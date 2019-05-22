/*

     Copyright (C) 2013 Proxmox Server Solutions GmbH

     Copyright: spiceterm is under GNU GPL, the GNU General Public License.

     This program is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License as published by
     the Free Software Foundation; version 2 dated June, 1991.

     This program is distributed in the hope that it will be useful,
     but WITHOUT ANY WARRANTY; without even the implied warranty of
     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
     GNU General Public License for more details.

     You should have received a copy of the GNU General Public License
     along with this program; if not, write to the Free Software
     Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
     02111-1307, USA.

     Author: Dietmar Maurer <dietmar@proxmox.com>

     Note: qlx drawing code is copied from spice-server test code.

*/

#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <wait.h>
#include <sys/select.h>
#include <sys/types.h>
#include <getopt.h>

#include <spice.h>
#include <spice/enums.h>
#include <spice/macros.h>
#include <spice/qxl_dev.h>
#include <spice/vd_agent.h>

#include "glyphs.h"

#include "spiceterm.h"

static int debug = 0;
    
#define DPRINTF(x, format, ...) { \
    if (x <= debug) { \
        printf("%s: " format "\n" , __FUNCTION__, ## __VA_ARGS__); \
    } \
}

#define MEM_SLOT_GROUP_ID 0


extern unsigned char color_table[];

/* these colours are from linux kernel drivers/char/vt.c */
/* the default colour table, for VGA+ colour systems */
int default_red[] = {0x00,0xaa,0x00,0xaa,0x00,0xaa,0x00,0xaa,
    0x55,0xff,0x55,0xff,0x55,0xff,0x55,0xff};
int default_grn[] = {0x00,0x00,0xaa,0x55,0x00,0x00,0xaa,0xaa,
    0x55,0x55,0xff,0xff,0x55,0x55,0xff,0xff};
int default_blu[] = {0x00,0x00,0x00,0x00,0xaa,0xaa,0xaa,0xaa,
    0x55,0x55,0x55,0x55,0xff,0xff,0xff,0xff};

/* Parts cribbed from spice-display.h/.c/qxl.c */

typedef struct SimpleSpiceUpdate {
    QXLCommandExt ext; // needs to be first member
    QXLDrawable drawable;
    QXLImage image;
    uint8_t *bitmap;
    int cache_id; // do not free bitmap if cache_id != 0
} SimpleSpiceUpdate;

static void 
spice_screen_destroy_update(SimpleSpiceUpdate *update)
{
    if (!update) {
        return;
    }
    if (update->drawable.clip.type != SPICE_CLIP_TYPE_NONE) {
        uint8_t *ptr = (uint8_t*)update->drawable.clip.data;
        free(ptr);
    }
    if (update->bitmap && !update->cache_id) {
        g_free(update->bitmap);
    }

    g_free(update);
}

static void 
release_qxl_command_ext(QXLCommandExt *ext)
{
    g_assert(ext != NULL);

    switch (ext->cmd.type) {
    case QXL_CMD_DRAW:
        spice_screen_destroy_update((void*)ext);
        break;
    case QXL_CMD_SURFACE:
        free(ext);
        break;
    case QXL_CMD_CURSOR: {
        QXLCursorCmd *cmd = (QXLCursorCmd *)(unsigned long)ext->cmd.data;
        if (cmd->type == QXL_CURSOR_SET) {
            free(cmd);
        }
        free(ext);
        break;
    }
    default:
        abort();
    }
}

static void 
release_resource(QXLInstance *qin, struct QXLReleaseInfoExt release_info)
{
    QXLCommandExt *ext = (QXLCommandExt*)(unsigned long)release_info.info->id;

    g_assert(release_info.group_id == MEM_SLOT_GROUP_ID);
    release_qxl_command_ext(ext);
}

static int unique = 0x0ffff + 1;

static void 
set_cmd(QXLCommandExt *ext, uint32_t type, QXLPHYSICAL data)
{
    ext->cmd.type = type;
    ext->cmd.data = data;
    ext->cmd.padding = 0;
    ext->group_id = MEM_SLOT_GROUP_ID;
    ext->flags = 0;
}

static void 
simple_set_release_info(QXLReleaseInfo *info, intptr_t ptr)
{
    info->id = ptr;
    //info->group_id = MEM_SLOT_GROUP_ID;
}

/* Note: push_command/get_command are called from different threads */

static void 
push_command(SpiceScreen *spice_screen, QXLCommandExt *ext)
{
    int need_wakeup = 1;

    g_mutex_lock(&spice_screen->command_mutex);

    while (spice_screen->commands_end - spice_screen->commands_start >= COMMANDS_SIZE) {
        g_cond_wait(&spice_screen->command_cond, &spice_screen->command_mutex);
    }

    g_assert(spice_screen->commands_end - spice_screen->commands_start < COMMANDS_SIZE);

    if ((spice_screen->commands_end - spice_screen->commands_start) > 0) {
        need_wakeup = 0;
    }

    spice_screen->commands[spice_screen->commands_end % COMMANDS_SIZE] = ext;
    spice_screen->commands_end++;

    if (need_wakeup) {
	spice_qxl_wakeup(&spice_screen->qxl_instance);
        //spice_screen->qxl_worker->wakeup(spice_screen->qxl_worker);
    }

    g_mutex_unlock(&spice_screen->command_mutex);

}

/* bitmap are freed, so they must be allocated with g_malloc */
static SimpleSpiceUpdate *
spice_screen_update_from_bitmap_cmd(uint32_t surface_id, QXLRect bbox, uint8_t *bitmap, int cache_id)
{
    SimpleSpiceUpdate *update;
    QXLDrawable *drawable;
    QXLImage *image;
    uint32_t bw, bh;

    bh = bbox.bottom - bbox.top;
    bw = bbox.right - bbox.left;

    update   = g_new0(SimpleSpiceUpdate, 1);
    update->bitmap = bitmap;
    drawable = &update->drawable;
    image    = &update->image;

    drawable->surface_id      = surface_id;

    drawable->bbox            = bbox;
    drawable->clip.type       = SPICE_CLIP_TYPE_NONE;
    drawable->effect          = QXL_EFFECT_OPAQUE;
    simple_set_release_info(&drawable->release_info, (intptr_t)update);
    drawable->type            = QXL_DRAW_COPY;
    drawable->surfaces_dest[0] = -1;
    drawable->surfaces_dest[1] = -1;
    drawable->surfaces_dest[2] = -1;

    drawable->u.copy.rop_descriptor  = SPICE_ROPD_OP_PUT;
    drawable->u.copy.src_bitmap      = (intptr_t)image;
    drawable->u.copy.src_area.right  = bw;
    drawable->u.copy.src_area.bottom = bh;

    if (cache_id) {
        QXL_SET_IMAGE_ID(image, QXL_IMAGE_GROUP_DEVICE, cache_id);
        image->descriptor.flags = SPICE_IMAGE_FLAGS_CACHE_ME;
        update->cache_id = cache_id;
    } else {
        QXL_SET_IMAGE_ID(image, QXL_IMAGE_GROUP_DEVICE, ++unique);
    }
    image->descriptor.type   = SPICE_IMAGE_TYPE_BITMAP;
    image->bitmap.flags      = QXL_BITMAP_DIRECT | QXL_BITMAP_TOP_DOWN;
    image->bitmap.stride     = bw * 4;
    image->descriptor.width  = image->bitmap.x = bw;
    image->descriptor.height = image->bitmap.y = bh;
    image->bitmap.data = (intptr_t)bitmap;
    image->bitmap.palette = 0;
    image->bitmap.format = SPICE_BITMAP_FMT_32BIT;

    set_cmd(&update->ext, QXL_CMD_DRAW, (intptr_t)drawable);

    return update;
}

static SimpleSpiceUpdate *
spice_screen_draw_char_cmd(SpiceScreen *spice_screen, int x, int y, int c, 
                           int fg, int bg, gboolean uline)
{
    int top, left;
    uint8_t *dst;
    uint8_t *bitmap = NULL;
    int bw, bh;
    int i, j;
    QXLRect bbox;
    int cache_id = 0;
    CachedImage *ce;

    if (!uline && c < 256) {
        cache_id = ((fg << 12) | (bg << 8) | (c & 255)) & 0x0ffff;
        if ((ce = (CachedImage *)g_hash_table_lookup(spice_screen->image_cache, &cache_id))) {
            bitmap = ce->bitmap;
        }
    }

    left = x*8;
    top = y*16;
 
    bw       = 8;
    bh       = 16;

    if (!bitmap) {
        bitmap = dst = g_malloc(bw * bh * 4);
        
        unsigned char *data = vt_font_data + c*16;
        unsigned char d = *data;

        g_assert(fg >= 0 && fg < 16);
        g_assert(bg >= 0 && bg < 16);

        unsigned char fgc_red = default_red[color_table[fg]];
        unsigned char fgc_blue = default_blu[color_table[fg]];
        unsigned char fgc_green = default_grn[color_table[fg]];
        unsigned char bgc_red = default_red[color_table[bg]];
        unsigned char bgc_blue = default_blu[color_table[bg]];
        unsigned char bgc_green = default_grn[color_table[bg]];

        for (j = 0; j < 16; j++) {
            gboolean ul = (j == 14) && uline;
            for (i = 0; i < 8; i++) {
                if (i == 0) {
                    d=*data;
                    data++;
                }
                if (ul || d&0x80) {
                    *(dst) = fgc_blue;
                    *(dst+1) = fgc_green;
                    *(dst+2) = fgc_red;
                    *(dst+3) = 0;
                } else {
                    *(dst) = bgc_blue;
                    *(dst+1) = bgc_green;
                    *(dst+2) = bgc_red;
                    *(dst+3) = 0;
                }
                d<<=1;
                dst += 4;
            }
        }

	if (cache_id != 0) {
	    ce = g_new(CachedImage, 1);
	    ce->cache_id = cache_id;
	    ce->bitmap = bitmap;
	    g_hash_table_insert(spice_screen->image_cache, &ce->cache_id, ce);
	}
    }

    bbox.left = left; bbox.top = top;
    bbox.right = left + bw; bbox.bottom = top + bh;

    return spice_screen_update_from_bitmap_cmd(0, bbox, bitmap, cache_id);
}

void 
spice_screen_scroll(SpiceScreen *spice_screen, int x1, int y1, 
                    int x2, int y2, int src_x, int src_y)
{
    SimpleSpiceUpdate *update;
    QXLDrawable *drawable;
    QXLRect bbox;

    int surface_id = 0;

    update   = g_new0(SimpleSpiceUpdate, 1);
    drawable = &update->drawable;

    bbox.left = x1;
    bbox.top = y1;
    bbox.right = x2;
    bbox.bottom = y2;

    drawable->surface_id = surface_id;

    drawable->bbox            = bbox;
    drawable->clip.type       = SPICE_CLIP_TYPE_NONE;
    drawable->effect          = QXL_EFFECT_OPAQUE;
    simple_set_release_info(&drawable->release_info, (intptr_t)update);
    drawable->type            = QXL_COPY_BITS;
    drawable->surfaces_dest[0] = -1;
    drawable->surfaces_dest[1] = -1;
    drawable->surfaces_dest[2] = -1;

    drawable->u.copy_bits.src_pos.x = src_x;
    drawable->u.copy_bits.src_pos.y = src_y;

    set_cmd(&update->ext, QXL_CMD_DRAW, (intptr_t)drawable);

    push_command(spice_screen, &update->ext);
}

void 
spice_screen_clear(SpiceScreen *spice_screen, int x1, int y1, int x2, int y2)
{
    SimpleSpiceUpdate *update;
    QXLDrawable *drawable;
    QXLRect bbox;

    int surface_id = 0;

    update   = g_new0(SimpleSpiceUpdate, 1);
    drawable = &update->drawable;

    bbox.left = x1;
    bbox.top = y1;
    bbox.right = x2;
    bbox.bottom = y2;

    drawable->surface_id = surface_id;

    drawable->bbox            = bbox;
    drawable->clip.type       = SPICE_CLIP_TYPE_NONE;
    drawable->effect          = QXL_EFFECT_OPAQUE;
    simple_set_release_info(&drawable->release_info, (intptr_t)update);
    drawable->type            = QXL_DRAW_BLACKNESS;
    drawable->surfaces_dest[0] = -1;
    drawable->surfaces_dest[1] = -1;
    drawable->surfaces_dest[2] = -1;

    set_cmd(&update->ext, QXL_CMD_DRAW, (intptr_t)drawable);

    push_command(spice_screen, &update->ext);
}

static void 
create_primary_surface(SpiceScreen *spice_screen, uint32_t width, 
                       uint32_t height)
{
    QXLDevSurfaceCreate surface = { 0, };

    g_assert(height > 0);
    g_assert(width > 0);

    if (height > MAX_HEIGHT)
        height = MAX_HEIGHT;

    if (width > MAX_WIDTH)
        width = MAX_WIDTH;

    surface.format     = SPICE_SURFACE_FMT_32_xRGB;
    surface.width      = spice_screen->primary_width = width;
    surface.height     = spice_screen->primary_height = height;
    surface.stride     = -width * 4; /* negative? */
    surface.mouse_mode = TRUE; /* unused by red_worker */
    surface.flags      = 0;
    surface.type       = 0;    /* unused by red_worker */
    surface.position   = 0;    /* unused by red_worker */
    surface.mem        = (uint64_t)&spice_screen->primary_surface;
    surface.group_id   = MEM_SLOT_GROUP_ID;

    spice_screen->width = width;
    spice_screen->height = height;

    spice_screen->cursor_set = 0;

    spice_qxl_create_primary_surface(&spice_screen->qxl_instance, 0, &surface);
}

QXLDevMemSlot slot = {
    .slot_group_id = MEM_SLOT_GROUP_ID,
    .slot_id = 0,
    .generation = 0,
    .virt_start = 0,
    .virt_end = ~0,
    .addr_delta = 0,
    .qxl_ram_size = ~0,
};

static void 
attache_worker(QXLInstance *qin, QXLWorker *_qxl_worker)
{
    SpiceScreen *spice_screen = SPICE_CONTAINEROF(qin, SpiceScreen, qxl_instance);

    if (spice_screen->qxl_worker) {
        g_assert_not_reached();
    }
 
    spice_screen->qxl_worker = _qxl_worker;
    spice_qxl_add_memslot(&spice_screen->qxl_instance, &slot);
    create_primary_surface(spice_screen, spice_screen->width, spice_screen->height);
    spice_server_vm_start(spice_screen->server);
}

static void 
set_compression_level(QXLInstance *qin, int level)
{
    /* not used */
}

static void 
set_mm_time(QXLInstance *qin, uint32_t mm_time)
{
    /* not used */
}

static void 
get_init_info(QXLInstance *qin, QXLDevInitInfo *info)
{
    memset(info, 0, sizeof(*info));
    info->num_memslots = 1;
    info->num_memslots_groups = 1;
    info->memslot_id_bits = 1;
    info->memslot_gen_bits = 1;
    info->n_surfaces = 1;
}

/* called from spice_server thread (i.e. red_worker thread) */
static int 
get_command(QXLInstance *qin, struct QXLCommandExt *ext)
{
    SpiceScreen *spice_screen = SPICE_CONTAINEROF(qin, SpiceScreen, qxl_instance);
    int res = FALSE;

    g_mutex_lock(&spice_screen->command_mutex);
    
    if ((spice_screen->commands_end - spice_screen->commands_start) == 0) {
        res = FALSE;
        goto ret;
    }

    *ext = *spice_screen->commands[spice_screen->commands_start % COMMANDS_SIZE];
    g_assert(spice_screen->commands_start < spice_screen->commands_end);
    spice_screen->commands_start++;
    g_cond_signal(&spice_screen->command_cond);

    res = TRUE;

ret:
    g_mutex_unlock(&spice_screen->command_mutex);
    return res;
}

void
discard_pending_commands(SpiceScreen *spice_screen) 
{
    int pos;

    g_mutex_lock(&spice_screen->command_mutex);
    for (pos = spice_screen->commands_start; pos < spice_screen->commands_end; pos++) {
        release_qxl_command_ext(spice_screen->commands[pos % COMMANDS_SIZE]);
    }
    spice_screen->commands_start = spice_screen->commands_end;
    g_mutex_unlock(&spice_screen->command_mutex);
}

static int 
req_cmd_notification(QXLInstance *qin)
{
    //SpiceScreen *spice_screen = SPICE_CONTAINEROF(qin, SpiceScreen, qxl_instance);
    //spice_screen->core->timer_start(spice_screen->wakeup_timer, spice_screen->wakeup_ms);

    return TRUE;
}

#define CURSOR_WIDTH 8
#define CURSOR_HEIGHT 16

static struct {
    QXLCursor cursor;
    uint8_t data[CURSOR_WIDTH * CURSOR_HEIGHT * 4]; // 32bit per pixel
} cursor;

static void 
cursor_init()
{
    cursor.cursor.header.unique = 0;
    cursor.cursor.header.type = SPICE_CURSOR_TYPE_COLOR32;
    cursor.cursor.header.width = CURSOR_WIDTH;
    cursor.cursor.header.height = CURSOR_HEIGHT;
    cursor.cursor.header.hot_spot_x = 0;
    cursor.cursor.header.hot_spot_y = 0;
    cursor.cursor.data_size = CURSOR_WIDTH * CURSOR_HEIGHT * 4;

    // X drivers addes it to the cursor size because it could be
    // cursor data information or another cursor related stuffs.
    // Otherwise, the code will break in client/cursor.cpp side,
    // that expect the data_size plus cursor information.
    // Blame cursor protocol for this. :-)
    cursor.cursor.data_size += 128;
    cursor.cursor.chunk.data_size = cursor.cursor.data_size;
    cursor.cursor.chunk.prev_chunk = cursor.cursor.chunk.next_chunk = 0;
}

static int 
get_cursor_command(QXLInstance *qin, struct QXLCommandExt *ext)
{
    SpiceScreen *spice_screen = SPICE_CONTAINEROF(qin, SpiceScreen, qxl_instance);
 
    QXLCursorCmd *cursor_cmd;
    QXLCommandExt *cmd;

    if (spice_screen->cursor_set) 
        return FALSE;
        
    spice_screen->cursor_set = 1;
    
    cmd = calloc(sizeof(QXLCommandExt), 1);
    cursor_cmd = calloc(sizeof(QXLCursorCmd), 1);

    cursor_cmd->release_info.id = (unsigned long)cmd;

    cursor_cmd->type = QXL_CURSOR_SET;
    cursor_cmd->u.set.position.x = 0;
    cursor_cmd->u.set.position.y = 0;
    cursor_cmd->u.set.visible = TRUE;
    cursor_cmd->u.set.shape = (unsigned long)&cursor;
    // white rect as cursor
    memset(cursor.data, 255, sizeof(cursor.data));

    cmd->cmd.data = (unsigned long)cursor_cmd;
    cmd->cmd.type = QXL_CMD_CURSOR;
    cmd->group_id = MEM_SLOT_GROUP_ID;
    cmd->flags = 0;
    *ext = *cmd;

    return TRUE;
}

static int 
req_cursor_notification(QXLInstance *qin)
{
    /* not used */

    return TRUE;
}

static void 
notify_update(QXLInstance *qin, uint32_t update_id)
{
    /* not used */
}

static int 
flush_resources(QXLInstance *qin)
{
    /* not used */

    return TRUE;
}

static int 
client_monitors_config(QXLInstance *qin, VDAgentMonitorsConfig *monitors_config)
{
    /* not used */

    return 0;
}

static void 
set_client_capabilities(QXLInstance *qin, uint8_t client_present, 
                        uint8_t caps[58])
{
    SpiceScreen *spice_screen = SPICE_CONTAINEROF(qin, SpiceScreen, qxl_instance);

    DPRINTF(1, "present %d caps %d", client_present, caps[0]);

    if (spice_screen->on_client_connected && client_present) {
        spice_screen->on_client_connected(spice_screen);
    }
    if (spice_screen->on_client_disconnected && !client_present) {
        spice_screen->on_client_disconnected(spice_screen);
    }
}

static int client_count = 0;

static void 
client_connected(SpiceScreen *spice_screen)
{
    client_count++;

    DPRINTF(1, "client_count = %d", client_count);
}

static void 
client_disconnected(SpiceScreen *spice_screen)
{    
    if (client_count > 0) {
        client_count--;
        DPRINTF(1, "client_count = %d", client_count);
        exit(0); // fixme: cleanup?
    }
}

static void 
do_conn_timeout(void *opaque)
{
    // SpiceScreen *spice_screen = opaque;

    if (client_count <= 0) {
        printf("connection timeout - stopping server\n");
        exit (0); // fixme: cleanup?
    }
}


QXLInterface display_sif = {
    .base = {
        .type = SPICE_INTERFACE_QXL,
        .description = "spiceterm display server",
        .major_version = SPICE_INTERFACE_QXL_MAJOR,
        .minor_version = SPICE_INTERFACE_QXL_MINOR
    },
    .attache_worker = attache_worker,
    .set_compression_level = set_compression_level,
    .set_mm_time = set_mm_time,
    .get_init_info = get_init_info,

    /* the callbacks below are called from spice server thread context */
    .get_command = get_command,
    .req_cmd_notification = req_cmd_notification,
    .release_resource = release_resource,
    .get_cursor_command = get_cursor_command,
    .req_cursor_notification = req_cursor_notification,
    .notify_update = notify_update,
    .flush_resources = flush_resources,
    .client_monitors_config = client_monitors_config,
    .set_client_capabilities = set_client_capabilities,
};


void 
spice_screen_draw_char(SpiceScreen *spice_screen, int x, int y, gunichar2 ch, 
                       TextAttributes attrib)
{
    int fg, bg;

    int invers;
    if (attrib.invers) {
        invers = attrib.selected ? 0 : 1;
    } else {
        invers = attrib.selected ? 1 : 0;
    }        

    if (invers) {
        bg = attrib.fgcol;
        fg = attrib.bgcol;
    } else {
        bg = attrib.bgcol;
        fg = attrib.fgcol;
    }

    if (attrib.bold) {
        fg += 8;
    }

    // unsuported attributes = (attrib.blink || attrib.unvisible)

    int c = vt_fontmap[ch];

    SimpleSpiceUpdate *update;
    update = spice_screen_draw_char_cmd(spice_screen, x, y, c, fg, bg, attrib.uline);
    push_command(spice_screen, &update->ext);
}

SpiceScreen *
spice_screen_new(SpiceCoreInterface *core, uint32_t width, uint32_t height, 
                 SpiceTermOptions *opts)
{
    SpiceScreen *spice_screen = g_new0(SpiceScreen, 1);
    SpiceServer* server = spice_server_new();
    char *x509_key_file = "/etc/pve/local/pve-ssl.key";
    char *x509_cert_file = "/etc/pve/local/pve-ssl.pem";
    char *x509_cacert_file = "/etc/pve/pve-root-ca.pem";
    char *x509_key_password = NULL;
    char *x509_dh_file = NULL;
    char *tls_ciphers = "HIGH";

    spice_screen->width = width;
    spice_screen->height = height;

    g_cond_init(&spice_screen->command_cond);
    g_mutex_init(&spice_screen->command_mutex);

    spice_screen->on_client_connected = client_connected,
    spice_screen->on_client_disconnected = client_disconnected,

    spice_screen->qxl_instance.base.sif = &display_sif.base;
    spice_screen->qxl_instance.id = 0;

    spice_screen->core = core;
    spice_screen->server = server;

    if (opts->addr) {
        printf("listening on '%s:%d' (TLS)\n", opts->addr, opts->port);
        spice_server_set_addr(server, opts->addr, 0);
    } else {
        printf("listening on '*:%d' (TLS)\n", opts->port);
    }

    // spice_server_set_port(spice_server, port);
    //spice_server_set_image_compression(server, SPICE_IMAGE_COMPRESS_OFF);

    spice_server_set_tls(server, opts->port,
                         x509_cacert_file,
                         x509_cert_file,
                         x509_key_file,
                         x509_key_password,
                         x509_dh_file,
                         tls_ciphers);

    if (opts->noauth) {
        spice_server_set_noauth(server);
    } else {
        char *ticket = getenv("SPICE_TICKET");
        if (ticket) {
            spice_server_set_ticket(server, ticket, 300, 0, 0);
        }
    }

    int res = spice_server_init(server, core);
    if (res != 0) {
        g_error("spice_server_init failed, res = %d\n", res);
    }

    cursor_init();

    if (opts->timeout > 0) {
        spice_screen->conn_timeout_timer = core->timer_add(do_conn_timeout, spice_screen);
        spice_screen->core->timer_start(spice_screen->conn_timeout_timer, opts->timeout*1000);
    }

    spice_server_add_interface(spice_screen->server, &spice_screen->qxl_instance.base);

    return spice_screen;
}

void 
spice_screen_resize(SpiceScreen *spice_screen, uint32_t width,
                    uint32_t height)
{
    if (spice_screen->width == width && spice_screen->height == height) {
        return;
    }

    discard_pending_commands(spice_screen);

    spice_qxl_destroy_primary_surface(&spice_screen->qxl_instance, 0);
 
    create_primary_surface(spice_screen, width, height);

    spice_screen_clear(spice_screen, 0, 0, width, height);
}
