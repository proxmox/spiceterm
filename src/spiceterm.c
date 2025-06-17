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

     Note: most of the code here is copied from vncterm (which is
     also written by me).

*/

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <locale.h>
#include <netdb.h>
#include <pty.h> /* for openpty and forkpty */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "spiceterm.h"

#include <glib.h>
#include <spice.h>
#include <spice/enums.h>
#include <spice/macros.h>
#include <spice/qxl_dev.h>

#include "event_loop.h"
#include "translations.h"

static int debug = 0;

#define DPRINTF(x, format, ...)                                                                    \
    {                                                                                              \
        if (x <= debug) {                                                                          \
            printf("%s: " format "\n", __FUNCTION__, ##__VA_ARGS__);                               \
        }                                                                                          \
    }

#define TERM "xterm"

#define TERMIDCODE "[?1;2c" // vt100 ID

/* these colours are from linux kernel drivers/char/vt.c */

unsigned char color_table[] = {0, 4, 2, 6, 1, 5, 3, 7, 8, 12, 10, 14, 9, 13, 11, 15};

static void draw_char_at(spiceTerm *vt, int x, int y, gunichar2 ch, TextAttributes attrib) {
    if (x < 0 || y < 0 || x >= vt->width || y >= vt->height) {
        return;
    }

    spice_screen_draw_char(vt->screen, x, y, ch, attrib);
}

static void spiceterm_update_xy(spiceTerm *vt, int x, int y) {
    if (x < 0 || y < 0 || x >= vt->width || y >= vt->height) {
        return;
    }

    int y1 = (vt->y_base + y) % vt->total_height;
    int y2 = y1 - vt->y_displ;
    if (y2 < 0) {
        y2 += vt->total_height;
    }
    if (y2 < vt->height) {
        TextCell *c = &vt->cells[y1 * vt->width + x];
        draw_char_at(vt, x, y2, c->ch, c->attrib);
    }
}

static void spiceterm_clear_xy(spiceTerm *vt, int x, int y) {
    if (x < 0 || y < 0 || x >= vt->width || y >= vt->height) {
        return;
    }

    int y1 = (vt->y_base + y) % vt->total_height;
    int y2 = y1 - vt->y_displ;
    if (y2 < 0) {
        y2 += vt->total_height;
    }
    if (y2 < vt->height) {
        TextCell *c = &vt->cells[y1 * vt->width + x];
        c->ch = ' ';
        c->attrib = vt->default_attrib;
        c->attrib.fgcol = vt->cur_attrib.fgcol;
        c->attrib.bgcol = vt->cur_attrib.bgcol;

        draw_char_at(vt, x, y, c->ch, c->attrib);
    }
}

void spiceterm_toggle_marked_cell(spiceTerm *vt, int pos) {
    int x = (pos % vt->width);
    int y = (pos / vt->width);

    if (x < 0 || y < 0 || x >= vt->width || y >= vt->height) {
        return;
    }

    int y1 = (vt->y_displ + y) % vt->total_height;

    TextCell *c = &vt->cells[y1 * vt->width + x];
    c->attrib.selected = c->attrib.selected ? 0 : 1;

    if (y < vt->height) {
        draw_char_at(vt, x, y, c->ch, c->attrib);
    }
}

static void spiceterm_show_cursor(spiceTerm *vt, int show) {
    int x = vt->cx;
    if (x >= vt->width) {
        x = vt->width - 1;
    }

    int y1 = (vt->y_base + vt->cy) % vt->total_height;
    int y = y1 - vt->y_displ;
    if (y < 0) {
        y += vt->total_height;
    }

    if (y < vt->height) {

        TextCell *c = &vt->cells[y1 * vt->width + x];

        if (show) {
            TextAttributes attrib = vt->default_attrib;
            attrib.invers = !(attrib.invers); /* invert fg and bg */
            draw_char_at(vt, x, y, c->ch, attrib);
        } else {
            draw_char_at(vt, x, y, c->ch, c->attrib);
        }
    }
}

void spiceterm_refresh(spiceTerm *vt) {
    int x, y, y1;

    y1 = vt->y_displ;
    for (y = 0; y < vt->height; y++) {
        TextCell *c = vt->cells + y1 * vt->width;
        for (x = 0; x < vt->width; x++) {
            draw_char_at(vt, x, y, c->ch, c->attrib);
            c++;
        }
        if (++y1 == vt->total_height) {
            y1 = 0;
        }
    }

    spiceterm_show_cursor(vt, 1);
}

static void spiceterm_clear_screen(spiceTerm *vt) {
    int x, y;

    for (y = 0; y <= vt->height; y++) {
        int y1 = (vt->y_base + y) % vt->total_height;
        TextCell *c = &vt->cells[y1 * vt->width];
        for (x = 0; x < vt->width; x++) {
            c->ch = ' ';
            c->attrib = vt->default_attrib;
            c->attrib.fgcol = vt->cur_attrib.fgcol;
            c->attrib.bgcol = vt->cur_attrib.bgcol;

            c++;
        }
    }

    spice_screen_clear(vt->screen, 0, 0, vt->screen->primary_width, vt->screen->primary_height);
}

void spiceterm_unselect_all(spiceTerm *vt) {
    int x, y, y1;

    y1 = vt->y_displ;
    for (y = 0; y < vt->total_height; y++) {
        TextCell *c = vt->cells + y1 * vt->width;
        for (x = 0; x < vt->width; x++) {
            if (c->attrib.selected) {
                c->attrib.selected = 0;
                if (y < vt->height) {
                    draw_char_at(vt, x, y, c->ch, c->attrib);
                }
            }
            c++;
        }
        if (++y1 == vt->total_height) {
            y1 = 0;
        }
    }
}

static void spiceterm_scroll_down(spiceTerm *vt, int top, int bottom, int lines) {
    if ((top + lines) >= bottom) {
        lines = bottom - top - 1;
    }

    if (top < 0 || bottom > vt->height || top >= bottom || lines < 1) {
        return;
    }

    int i;
    for (i = bottom - top - lines - 1; i >= 0; i--) {
        int src = ((vt->y_base + top + i) % vt->total_height) * vt->width;
        int dst = ((vt->y_base + top + lines + i) % vt->total_height) * vt->width;

        memmove(vt->cells + dst, vt->cells + src, vt->width * sizeof(TextCell));
    }

    for (i = 0; i < lines; i++) {
        int j;
        TextCell *c = vt->cells + ((vt->y_base + top + i) % vt->total_height) * vt->width;
        for (j = 0; j < vt->width; j++) {
            c->attrib = vt->default_attrib;
            c->ch = ' ';
            c++;
        }
    }

    int h = lines * 16;
    int y0 = top * 16;
    int y1 = y0 + h;
    int y2 = bottom * 16;

    spice_screen_scroll(vt->screen, 0, y1, vt->screen->primary_width, y2, 0, y0);
    spice_screen_clear(vt->screen, 0, y0, vt->screen->primary_width, y1);
}

static void spiceterm_scroll_up(spiceTerm *vt, int top, int bottom, int lines, int moveattr) {
    if ((top + lines) >= bottom) {
        lines = bottom - top - 1;
    }

    if (top < 0 || bottom > vt->height || top >= bottom || lines < 1) {
        return;
    }

    int h = lines * 16;
    int y0 = top * 16;
    int y1 = (top + lines) * 16;
    int y2 = bottom * 16;

    spice_screen_scroll(vt->screen, 0, y0, vt->screen->primary_width, y2 - h, 0, y1);
    spice_screen_clear(vt->screen, 0, y2 - h, vt->screen->primary_width, y2);

    if (!moveattr) {
        return;
    }

    // move attributes

    int i;
    for (i = 0; i < (bottom - top - lines); i++) {
        int dst = ((vt->y_base + top + i) % vt->total_height) * vt->width;
        int src = ((vt->y_base + top + lines + i) % vt->total_height) * vt->width;

        memmove(vt->cells + dst, vt->cells + src, vt->width * sizeof(TextCell));
    }

    for (i = 1; i <= lines; i++) {
        int j;
        TextCell *c = vt->cells + ((vt->y_base + bottom - i) % vt->total_height) * vt->width;
        for (j = 0; j < vt->width; j++) {
            c->attrib = vt->default_attrib;
            c->ch = ' ';
            c++;
        }
    }
}

void spiceterm_virtual_scroll(spiceTerm *vt, int lines) {
    if (vt->altbuf || lines == 0) {
        return;
    }

    if (lines < 0) {
        lines = -lines;
        int i = vt->scroll_height;
        if (i > vt->total_height - vt->height) {
            i = vt->total_height - vt->height;
        }
        int y1 = vt->y_base - i;
        if (y1 < 0) {
            y1 += vt->total_height;
        }
        for (i = 0; i < lines; i++) {
            if (vt->y_displ == y1) {
                break;
            }
            if (--vt->y_displ < 0) {
                vt->y_displ = vt->total_height - 1;
            }
        }
    } else {
        int i;
        for (i = 0; i < lines; i++) {
            if (vt->y_displ == vt->y_base) {
                break;
            }
            if (++vt->y_displ == vt->total_height) {
                vt->y_displ = 0;
            }
        }
    }

    spiceterm_refresh(vt);
}

void spiceterm_respond_esc(spiceTerm *vt, const char *esc) {
    int len = strlen(esc);
    int i;

    if (vt->ibuf_count < (IBUFSIZE - 1 - len)) {
        vt->ibuf[vt->ibuf_count++] = 27;
        for (i = 0; i < len; i++) {
            vt->ibuf[vt->ibuf_count++] = esc[i];
        }
    } else {
        fprintf(stderr, "input buffer oferflow\n");
        return;
    }
}

void spiceterm_respond_data(spiceTerm *vt, int len, uint8_t *data) {
    int i;

    if (vt->ibuf_count < (IBUFSIZE - len)) {
        for (i = 0; i < len; i++) {
            vt->ibuf[vt->ibuf_count++] = data[i];
        }
    } else {
        fprintf(stderr, "input buffer oferflow\n");
        return;
    }
}

static void spiceterm_put_lf(spiceTerm *vt) {
    if (vt->cy + 1 == vt->region_bottom) {

        if (vt->altbuf || vt->region_top != 0 || vt->region_bottom != vt->height) {
            spiceterm_scroll_up(vt, vt->region_top, vt->region_bottom, 1, 1);
            return;
        }

        if (vt->y_displ == vt->y_base) {
            spiceterm_scroll_up(vt, vt->region_top, vt->region_bottom, 1, 0);
        }

        if (vt->y_displ == vt->y_base) {
            if (++vt->y_displ == vt->total_height) {
                vt->y_displ = 0;
            }
        }

        if (++vt->y_base == vt->total_height) {
            vt->y_base = 0;
        }

        if (vt->scroll_height < vt->total_height) {
            vt->scroll_height++;
        }

        int y1 = (vt->y_base + vt->height - 1) % vt->total_height;
        TextCell *c = &vt->cells[y1 * vt->width];
        int x;
        for (x = 0; x < vt->width; x++) {
            c->ch = ' ';
            c->attrib = vt->default_attrib;
            c++;
        }

        // fprintf (stderr, "BASE: %d DISPLAY %d\n", vt->y_base, vt->y_displ);

    } else if (vt->cy < vt->height - 1) {
        vt->cy += 1;
    }
}

static void spiceterm_csi_m(spiceTerm *vt) {
    int i;

    for (i = 0; i < vt->esc_count; i++) {
        switch (vt->esc_buf[i]) {
        case 0: /* reset all console attributes to default */
            vt->cur_attrib = vt->default_attrib;
            break;
        case 1:
            vt->cur_attrib.bold = 1;
            break;
        case 4:
            vt->cur_attrib.uline = 1;
            break;
        case 5:
            vt->cur_attrib.blink = 1;
            break;
        case 7:
            vt->cur_attrib.invers = 1;
            break;
        case 8:
            vt->cur_attrib.unvisible = 1;
            break;
        case 10:
            vt->cur_enc = LAT1_MAP;
            // fixme: dispaly controls = 0 ?
            // fixme: toggle meta = 0 ?
            break;
        case 11:
            vt->cur_enc = IBMPC_MAP;
            // fixme: dispaly controls = 1 ?
            // fixme: toggle meta = 0 ?
            break;
        case 12:
            vt->cur_enc = IBMPC_MAP;
            // fixme: dispaly controls = 1 ?
            // fixme: toggle meta = 1 ?
            break;
        case 22:
            vt->cur_attrib.bold = 0;
            break;
        case 24:
            vt->cur_attrib.uline = 0;
            break;
        case 25:
            vt->cur_attrib.blink = 0;
            break;
        case 27:
            vt->cur_attrib.invers = 0;
            break;
        case 28:
            vt->cur_attrib.unvisible = 0;
            break;
        case 30:
        case 31:
        case 32:
        case 33:
        case 34:
        case 35:
        case 36:
        case 37:
            /* set foreground color */
            vt->cur_attrib.fgcol = color_table[vt->esc_buf[i] - 30];
            break;
        case 38:
            /* reset color to default, enable underline */
            vt->cur_attrib.fgcol = vt->default_attrib.fgcol;
            vt->cur_attrib.uline = 1;
            break;
        case 39:
            /* reset color to default, disable underline */
            vt->cur_attrib.fgcol = vt->default_attrib.fgcol;
            vt->cur_attrib.uline = 0;
            break;
        case 40:
        case 41:
        case 42:
        case 43:
        case 44:
        case 45:
        case 46:
        case 47:
            /* set background color */
            vt->cur_attrib.bgcol = color_table[vt->esc_buf[i] - 40];
            break;
        case 49:
            /* reset background color */
            vt->cur_attrib.bgcol = vt->default_attrib.bgcol;
            break;
        default:
            fprintf(stderr, "unhandled ESC[%d m code\n", vt->esc_buf[i]);
            // fixme: implement
        }
    }
}

static void spiceterm_save_cursor(spiceTerm *vt) {
    vt->cx_saved = vt->cx;
    vt->cy_saved = vt->cy;
    vt->cur_attrib_saved = vt->cur_attrib;
    vt->charset_saved = vt->charset;
    vt->g0enc_saved = vt->g0enc;
    vt->g1enc_saved = vt->g1enc;
    vt->cur_enc_saved = vt->cur_enc;
}

static void spiceterm_restore_cursor(spiceTerm *vt) {
    vt->cx = vt->cx_saved;
    vt->cy = vt->cy_saved;
    vt->cur_attrib = vt->cur_attrib_saved;
    vt->charset = vt->charset_saved;
    vt->g0enc = vt->g0enc_saved;
    vt->g1enc = vt->g1enc_saved;
    vt->cur_enc = vt->cur_enc_saved;
}

static void spiceterm_set_alternate_buffer(spiceTerm *vt, int on_off) {
    int x, y;

    vt->y_displ = vt->y_base;

    if (on_off) {

        if (vt->altbuf) {
            return;
        }

        vt->altbuf = 1;

        /* alternate buffer & cursor */

        spiceterm_save_cursor(vt);
        /* save screen to altcels */
        for (y = 0; y < vt->height; y++) {
            int y1 = (vt->y_base + y) % vt->total_height;
            for (x = 0; x < vt->width; x++) {
                vt->altcells[y * vt->width + x] = vt->cells[y1 * vt->width + x];
            }
        }

        /* clear screen */
        for (y = 0; y <= vt->height; y++) {
            for (x = 0; x < vt->width; x++) {
                //     spiceterm_clear_xy(vt, x, y);
            }
        }

    } else {

        if (vt->altbuf == 0) {
            return;
        }

        vt->altbuf = 0;

        /* restore saved data */
        for (y = 0; y < vt->height; y++) {
            int y1 = (vt->y_base + y) % vt->total_height;
            for (x = 0; x < vt->width; x++) {
                vt->cells[y1 * vt->width + x] = vt->altcells[y * vt->width + x];
            }
        }

        spiceterm_restore_cursor(vt);
    }

    spiceterm_refresh(vt);
}

static void spiceterm_set_mode(spiceTerm *vt, int on_off) {
    int i;

    for (i = 0; i <= vt->esc_count; i++) {
        if (vt->esc_ques) { /* DEC private modes set/reset */
            switch (vt->esc_buf[i]) {
            case 10: /* X11 mouse reporting on/off */
            case 1000: /* SET_VT200_MOUSE */
            case 1002: /* xterm SET_BTN_EVENT_MOUSE */
                vt->report_mouse = on_off;
                break;
            case 1049: /* start/end special app mode (smcup/rmcup) */
                spiceterm_set_alternate_buffer(vt, on_off);
                break;
            case 25: /* Cursor on/off */
            case 9: /* X10 mouse reporting on/off */
            case 6: /* Origin relative/absolute */
            case 1: /* Cursor keys in appl mode*/
            case 5: /* Inverted screen on/off */
            case 7: /* Autowrap on/off */
            case 8: /* Autorepeat on/off */
                break;
            }
        } else { /* ANSI modes set/reset */
            // g_assert_not_reached();

            /* fixme: implement me */
        }
    }
}

static void spiceterm_gotoxy(spiceTerm *vt, int x, int y) {
    /* verify all boundaries */

    if (x < 0) {
        x = 0;
    }

    if (x >= vt->width) {
        x = vt->width - 1;
    }

    vt->cx = x;

    if (y < 0) {
        y = 0;
    }

    if (y >= vt->height) {
        y = vt->height - 1;
    }

    vt->cy = y;
}

static void debug_print_escape_buffer(
    spiceTerm *vt, const char *func, const char *prefix, const char *qes, gunichar2 ch
) {
    if (debug >= 1) {
        if (vt->esc_count == 0) {
            printf("%s:%s ESC[%s%c\n", func, prefix, qes, ch);
        } else if (vt->esc_count == 1) {
            printf("%s:%s ESC[%s%d%c\n", func, prefix, qes, vt->esc_buf[0], ch);
        } else {
            int i;
            printf("%s:%s ESC[%s%d", func, prefix, qes, vt->esc_buf[0]);
            for (i = 1; i < vt->esc_count; i++) {
                printf(";%d", vt->esc_buf[i]);
            }
            printf("%c\n", ch);
        }
    }
}

enum {
    ESnormal,
    ESesc,
    ESsquare,
    ESgetpars,
    ESgotpars,
    ESfunckey,
    EShash,
    ESsetG0,
    ESsetG1,
    ESpercent,
    ESignore,
    ESnonstd,
    ESpalette,
    ESidquery,
    ESosc1,
    ESosc2
};

static void spiceterm_putchar(spiceTerm *vt, gunichar2 ch) {
    int x, y, i, c;

    if (debug && !vt->tty_state) {
        DPRINTF(
            1, "CHAR:%2d: %4x '%c' (cur_enc %d) %d %d", vt->tty_state, ch, ch, vt->cur_enc, vt->cx,
            vt->cy
        );
    }

    switch (vt->tty_state) {
    case ESesc:
        vt->tty_state = ESnormal;
        switch (ch) {
        case '[':
            vt->tty_state = ESsquare;
            break;
        case ']':
            vt->tty_state = ESnonstd;
            break;
        case '%':
            vt->tty_state = ESpercent;
            break;
        case '7':
            spiceterm_save_cursor(vt);
            break;
        case '8':
            spiceterm_restore_cursor(vt);
            break;
        case '(':
            vt->tty_state = ESsetG0; // SET G0
            break;
        case ')':
            vt->tty_state = ESsetG1; // SET G1
            break;
        case 'M':
            /* cursor up (ri) */
            if (vt->cy == vt->region_top) {
                spiceterm_scroll_down(vt, vt->region_top, vt->region_bottom, 1);
            } else if (vt->cy > 0) {
                vt->cy--;
            }
            break;
        case '>':
            /* numeric keypad  - ignored */
            break;
        case '=':
            /* appl. keypad - ignored */
            break;
        default:
            DPRINTF(1, "got unhandled ESC%c  %d", ch, ch);
            break;
        }
        break;
    case ESnonstd: /* Operating System Controls */
        vt->tty_state = ESnormal;

        switch (ch) {
        case 'P': /* palette escape sequence */
            for (i = 0; i < MAX_ESC_PARAMS; i++) {
                vt->esc_buf[i] = 0;
            }

            vt->esc_count = 0;
            vt->tty_state = ESpalette;
            break;
        case 'R': /* reset palette */
            // fixme: reset_palette(vc);
            break;
        case '0':
        case '1':
        case '2':
        case '4':
            vt->osc_cmd = ch;
            vt->osc_textbuf[0] = 0;
            vt->tty_state = ESosc1;
            break;
        default:
            DPRINTF(1, "got unhandled OSC %c", ch);
            vt->tty_state = ESnormal;
            break;
        }
        break;
    case ESosc1:
        vt->tty_state = ESnormal;
        if (ch == ';') {
            vt->tty_state = ESosc2;
        } else {
            DPRINTF(1, "got illegal OSC sequence");
        }
        break;
    case ESosc2:
        if (ch != 0x9c && ch != 7) {
            int i = 0;
            while (vt->osc_textbuf[i]) {
                i++;
            }
            vt->osc_textbuf[i++] = ch;
            vt->osc_textbuf[i] = 0;
        } else {
            DPRINTF(1, "OSC:%c:%s", vt->osc_cmd, vt->osc_textbuf);
            vt->tty_state = ESnormal;
        }
        break;
    case ESpalette:
        if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F') || (ch >= 'a' && ch <= 'f')) {
            vt->esc_buf[vt->esc_count++] = (ch > '9' ? (ch & 0xDF) - 'A' + 10 : ch - '0');
            if (vt->esc_count == 7) {
                // fixme: this does not work - please test
                /*
                  rfbColourMap *cmap =&vt->screen->colourMap;

                  int i = color_table[vt->esc_buf[0]] * 3, j = 1;
                  cmap->data.bytes[i] = 16 * vt->esc_buf[j++];
                  cmap->data.bytes[i++] += vt->esc_buf[j++];
                  cmap->data.bytes[i] = 16 * vt->esc_buf[j++];
                  cmap->data.bytes[i++] += vt->esc_buf[j++];
                  cmap->data.bytes[i] = 16 * vt->esc_buf[j++];
                  cmap->data.bytes[i] += vt->esc_buf[j];
                */
                // set_palette(vc); ?

                vt->tty_state = ESnormal;
            }
        } else {
            vt->tty_state = ESnormal;
        }
        break;
    case ESsquare:
        for (i = 0; i < MAX_ESC_PARAMS; i++) {
            vt->esc_buf[i] = 0;
        }

        vt->esc_count = 0;
        vt->esc_has_par = 0;
        vt->tty_state = ESgetpars;

        if (ch == '>') {
            vt->tty_state = ESidquery;
            break;
        }

        if ((vt->esc_ques = (ch == '?'))) {
            break;
        }
    case ESgetpars:
        if (ch >= '0' && ch <= '9') {
            vt->esc_has_par = 1;
            if (vt->esc_count < MAX_ESC_PARAMS) {
                vt->esc_buf[vt->esc_count] = vt->esc_buf[vt->esc_count] * 10 + ch - '0';
            }
            break;
        } else if (ch == ';') {
            vt->esc_count++;
            break;
        } else {
            if (vt->esc_has_par) {
                vt->esc_count++;
            }
            vt->tty_state = ESgotpars;
        }
    case ESgotpars:

        vt->tty_state = ESnormal;

        char *qes = vt->esc_ques ? "?" : "";

        if (debug) {
            debug_print_escape_buffer(vt, __func__, "", qes, ch);
        }

        switch (ch) {
        case 'h':
            spiceterm_set_mode(vt, 1);
            break;
        case 'l':
            spiceterm_set_mode(vt, 0);
            break;
        case 'm':
            if (!vt->esc_count) {
                vt->esc_count++; // default parameter 0
            }
            spiceterm_csi_m(vt);
            break;
        case 'n':
            /* report cursor position */
            /* TODO: send ESC[row;colR */
            break;
        case 'A':
            /* move cursor up */
            if (vt->esc_buf[0] == 0) {
                vt->esc_buf[0] = 1;
            }
            spiceterm_gotoxy(vt, vt->cx, vt->cy - vt->esc_buf[0]);
            break;
        case 'B':
        case 'e':
            /* move cursor down */
            if (vt->esc_buf[0] == 0) {
                vt->esc_buf[0] = 1;
            }
            spiceterm_gotoxy(vt, vt->cx, vt->cy + vt->esc_buf[0]);
            break;
        case 'C':
        case 'a':
            /* move cursor right */
            if (vt->esc_buf[0] == 0) {
                vt->esc_buf[0] = 1;
            }
            spiceterm_gotoxy(vt, vt->cx + vt->esc_buf[0], vt->cy);
            break;
        case 'D':
            /* move cursor left */
            if (vt->esc_buf[0] == 0) {
                vt->esc_buf[0] = 1;
            }
            spiceterm_gotoxy(vt, vt->cx - vt->esc_buf[0], vt->cy);
            break;
        case 'G':
        case '`':
            /* move cursor to column */
            spiceterm_gotoxy(vt, vt->esc_buf[0] - 1, vt->cy);
            break;
        case 'd':
            /* move cursor to row */
            spiceterm_gotoxy(vt, vt->cx, vt->esc_buf[0] - 1);
            break;
        case 'f':
        case 'H':
            /* move cursor to row, column */
            spiceterm_gotoxy(vt, vt->esc_buf[1] - 1, vt->esc_buf[0] - 1);
            break;
        case 'J':
            switch (vt->esc_buf[0]) {
            case 0:
                /* clear to end of screen */
                for (y = vt->cy; y < vt->height; y++) {
                    for (x = 0; x < vt->width; x++) {
                        if (y == vt->cy && x < vt->cx) {
                            continue;
                        }
                        spiceterm_clear_xy(vt, x, y);
                    }
                }
                break;
            case 1:
                /* clear from beginning of screen */
                for (y = 0; y <= vt->cy; y++) {
                    for (x = 0; x < vt->width; x++) {
                        if (y == vt->cy && x > vt->cx) {
                            break;
                        }
                        spiceterm_clear_xy(vt, x, y);
                    }
                }
                break;
            case 2:
                /* clear entire screen */
                spiceterm_clear_screen(vt);
                break;
            }
            break;
        case 'K':
            switch (vt->esc_buf[0]) {
            case 0:
                /* clear to eol */
                for (x = vt->cx; x < vt->width; x++) {
                    spiceterm_clear_xy(vt, x, vt->cy);
                }
                break;
            case 1:
                /* clear from beginning of line */
                for (x = 0; x <= vt->cx; x++) {
                    spiceterm_clear_xy(vt, x, vt->cy);
                }
                break;
            case 2:
                /* clear entire line */
                for (x = 0; x < vt->width; x++) {
                    spiceterm_clear_xy(vt, x, vt->cy);
                }
                break;
            }
            break;
        case 'L':
            /* insert line */
            c = vt->esc_buf[0];

            if (c > vt->height - vt->cy) {
                c = vt->height - vt->cy;
            } else if (!c) {
                c = 1;
            }

            spiceterm_scroll_down(vt, vt->cy, vt->region_bottom, c);
            break;
        case 'M':
            /* delete line */
            c = vt->esc_buf[0];

            if (c > vt->height - vt->cy) {
                c = vt->height - vt->cy;
            } else if (!c) {
                c = 1;
            }

            spiceterm_scroll_up(vt, vt->cy, vt->region_bottom, c, 1);
            break;
        case 'T':
            /* scroll down */
            c = vt->esc_buf[0];
            if (!c) {
                c = 1;
            }
            spiceterm_scroll_down(vt, vt->region_top, vt->region_bottom, c);
            break;
        case 'S':
            /* scroll up */
            c = vt->esc_buf[0];
            if (!c) {
                c = 1;
            }
            spiceterm_scroll_up(vt, vt->region_top, vt->region_bottom, c, 1);
            break;
        case 'P':
            /* delete c character */
            c = vt->esc_buf[0];

            if (c > vt->width - vt->cx) {
                c = vt->width - vt->cx;
            } else if (!c) {
                c = 1;
            }

            for (x = vt->cx; x < vt->width - c; x++) {
                int y1 = (vt->y_base + vt->cy) % vt->total_height;
                TextCell *dst = &vt->cells[y1 * vt->width + x];
                TextCell *src = dst + c;
                *dst = *src;
                spiceterm_update_xy(vt, x + c, vt->cy);
                src->ch = ' ';
                src->attrib = vt->default_attrib;
                spiceterm_update_xy(vt, x, vt->cy);
            }
            break;
        case 's':
            /* save cursor position */
            spiceterm_save_cursor(vt);
            break;
        case 'u':
            /* restore cursor position */
            spiceterm_restore_cursor(vt);
            break;
        case 'X':
            /* erase c characters */
            c = vt->esc_buf[0];
            if (!c) {
                c = 1;
            }

            if (c > (vt->width - vt->cx)) {
                c = vt->width - vt->cx;
            }

            for (i = 0; i < c; i++) {
                spiceterm_clear_xy(vt, vt->cx + i, vt->cy);
            }
            break;
        case '@':
            /* insert c character */
            c = vt->esc_buf[0];
            if (c > (vt->width - vt->cx)) {
                c = vt->width - vt->cx;
            }
            if (!c) {
                c = 1;
            }

            for (x = vt->width - c; x >= vt->cx; x--) {
                int y1 = (vt->y_base + vt->cy) % vt->total_height;
                TextCell *src = &vt->cells[y1 * vt->width + x];
                TextCell *dst = src + c;
                *dst = *src;
                spiceterm_update_xy(vt, x + c, vt->cy);
                src->ch = ' ';
                src->attrib = vt->cur_attrib;
                spiceterm_update_xy(vt, x, vt->cy);
            }

            break;
        case 'r':
            /* set region */
            if (!vt->esc_buf[0]) {
                vt->esc_buf[0]++;
            }
            if (!vt->esc_buf[1]) {
                vt->esc_buf[1] = vt->height;
            }
            /* Minimum allowed region is 2 lines */
            if (vt->esc_buf[0] < vt->esc_buf[1] && vt->esc_buf[1] <= vt->height) {
                vt->region_top = vt->esc_buf[0] - 1;
                vt->region_bottom = vt->esc_buf[1];
                vt->cx = 0;
                vt->cy = vt->region_top;
                DPRINTF(1, "set region %d %d", vt->region_top, vt->region_bottom);
            }

            break;
        default:
            if (debug) {
                debug_print_escape_buffer(vt, __func__, " unhandled escape", qes, ch);
            }
            break;
        }
        vt->esc_ques = 0;
        break;
    case ESsetG0: // Set G0
        vt->tty_state = ESnormal;

        if (ch == '0') {
            vt->g0enc = GRAF_MAP;
        } else if (ch == 'B') {
            vt->g0enc = LAT1_MAP;
        } else if (ch == 'U') {
            vt->g0enc = IBMPC_MAP;
        } else if (ch == 'K') {
            vt->g0enc = USER_MAP;
        }

        if (vt->charset == 0) {
            vt->cur_enc = vt->g0enc;
        }

        break;
    case ESsetG1: // Set G1
        vt->tty_state = ESnormal;

        if (ch == '0') {
            vt->g1enc = GRAF_MAP;
        } else if (ch == 'B') {
            vt->g1enc = LAT1_MAP;
        } else if (ch == 'U') {
            vt->g1enc = IBMPC_MAP;
        } else if (ch == 'K') {
            vt->g1enc = USER_MAP;
        }

        if (vt->charset == 1) {
            vt->cur_enc = vt->g1enc;
        }

        break;
    case ESidquery: // vt100 query id
        vt->tty_state = ESnormal;

        if (ch == 'c') {
            DPRINTF(1, "ESC[>c   Query term ID");
            spiceterm_respond_esc(vt, TERMIDCODE);
        }
        break;
    case ESpercent:
        vt->tty_state = ESnormal;
        switch (ch) {
        case '@': /* defined in ISO 2022 */
            vt->utf8 = 0;
            break;
        case 'G': /* prelim official escape code */
        case '8': /* retained for compatibility */
            vt->utf8 = 1;
            break;
        }
        break;
    default: // ESnormal
        vt->tty_state = ESnormal;

        switch (ch) {
        case 0:
            break;
        case 7: /* alert aka. bell */
            // fixme:
            // rfbSendBell(vt->screen);
            break;
        case 8: /* backspace */
            if (vt->cx > 0) {
                vt->cx--;
            }
            break;
        case 9: /* tabspace */
            if (vt->cx + (8 - (vt->cx % 8)) > vt->width) {
                vt->cx = 0;
                spiceterm_put_lf(vt);
            } else {
                vt->cx = vt->cx + (8 - (vt->cx % 8));
            }
            break;
        case 10: /* LF,*/
        case 11: /* VT */
        case 12: /* FF */
            spiceterm_put_lf(vt);
            break;
        case 13: /* carriage return */
            vt->cx = 0;
            break;
        case 14:
            /* SI (shift in), select character set 1 */
            vt->charset = 1;
            vt->cur_enc = vt->g1enc;
            /* fixme: display controls = 1 */
            break;
        case 15:
            /* SO (shift out), select character set 0 */
            vt->charset = 0;
            vt->cur_enc = vt->g0enc;
            /* fixme: display controls = 0 */
            break;
        case 27: /* esc */
            vt->tty_state = ESesc;
            break;
        case 127: /* delete */
            /* ignore */
            break;
        case 128 + 27: /* csi */
            vt->tty_state = ESsquare;
            break;
        default:
            if (vt->cx >= vt->width) {
                /* line wrap */
                vt->cx = 0;
                spiceterm_put_lf(vt);
            }

            int y1 = (vt->y_base + vt->cy) % vt->total_height;
            TextCell *c = &vt->cells[y1 * vt->width + vt->cx];
            c->attrib = vt->cur_attrib;
            c->ch = ch;
            spiceterm_update_xy(vt, vt->cx, vt->cy);
            vt->cx++;
            break;
        }
        break;
    }
}

static int spiceterm_puts(spiceTerm *vt, const char *buf, int len) {
    gunichar2 tc;

    spiceterm_show_cursor(vt, 0);

    while (len) {
        unsigned char c = *buf;
        len--;
        buf++;

        if (vt->tty_state != ESnormal) {
            // never translate escape sequence
            tc = c;
        } else if (vt->utf8 && !vt->cur_enc) {

            if (c & 0x80) { // utf8 multi-byte sequence

                if (vt->utf_count > 0 && (c & 0xc0) == 0x80) {
                    // inside UTF8 sequence
                    vt->utf_char = (vt->utf_char << 6) | (c & 0x3f);
                    vt->utf_count--;
                    if (vt->utf_count == 0) {
                        if (vt->utf_char <= G_MAXUINT16) {
                            tc = vt->utf_char;
                        } else {
                            tc = 0;
                        }
                    } else {
                        continue;
                    }
                } else {
                    //  first char of a UTF8 sequence
                    if ((c & 0xe0) == 0xc0) {
                        vt->utf_count = 1;
                        vt->utf_char = (c & 0x1f);
                    } else if ((c & 0xf0) == 0xe0) {
                        vt->utf_count = 2;
                        vt->utf_char = (c & 0x0f);
                    } else if ((c & 0xf8) == 0xf0) {
                        vt->utf_count = 3;
                        vt->utf_char = (c & 0x07);
                    } else if ((c & 0xfc) == 0xf8) {
                        vt->utf_count = 4;
                        vt->utf_char = (c & 0x03);
                    } else if ((c & 0xfe) == 0xfc) {
                        vt->utf_count = 5;
                        vt->utf_char = (c & 0x01);
                    } else {
                        vt->utf_count = 0;
                    }

                    continue;
                }
            } else {
                // utf8 single byte
                tc = c;
                vt->utf_count = 0;
            }

        } else {
            // never translate controls
            if (c >= 32 && c != 127 && c != (128 + 27)) {
                tc = translations[vt->cur_enc][c & 0x0ff];
            } else {
                tc = c;
            }
        }

        spiceterm_putchar(vt, tc);
    }

    spiceterm_show_cursor(vt, 1);

    return len;
}

void spiceterm_update_watch_mask(spiceTerm *vt, gboolean writable) {
    g_assert(vt != NULL);

    int mask = SPICE_WATCH_EVENT_READ;

    if (writable) {
        mask |= SPICE_WATCH_EVENT_WRITE;
    }

    vt->screen->core->watch_update_mask(vt->screen->mwatch, mask);
}

static void mouse_report(spiceTerm *vt, int butt, int mrx, int mry) {
    char buf[8];

    sprintf(buf, "[M%c%c%c", (char)(' ' + butt), (char)('!' + mrx), (char)('!' + mry));

    spiceterm_respond_esc(vt, buf);

    spiceterm_update_watch_mask(vt, TRUE);
}

static void spiceterm_respond_unichar2(spiceTerm *vt, gunichar2 uc) {
    if (vt->utf8) {
        gchar buf[10];
        gint len = g_unichar_to_utf8(uc, buf);

        if (len > 0) {
            spiceterm_respond_data(vt, len, (uint8_t *)buf);
        }
    } else {
        uint8_t buf[1] = {(uint8_t)uc};
        spiceterm_respond_data(vt, 1, buf);
    }
}

void spiceterm_clear_selection(spiceTerm *vt) {
    DPRINTF(1, "mark_active = %d", vt->mark_active);

    vt->mark_active = 0;
    if (vt->selection) {
        free(vt->selection);
    }
    vt->selection = NULL;

    spiceterm_unselect_all(vt);
}

void spiceterm_motion_event(spiceTerm *vt, uint32_t x, uint32_t y, uint32_t buttons) {
    DPRINTF(1, "mask=%08x x=%d y=%d", buttons, x, y);

    static int last_mask = 0;
    static int sel_start_pos = 0;
    static int sel_end_pos = 0;
    static int button2_released = 1;

    int i;
    int cx = x / 8;
    int cy = y / 16;

    if (cx < 0) {
        cx = 0;
    }
    if (cx >= vt->width) {
        cx = vt->width - 1;
    }
    if (cy < 0) {
        cy = 0;
    }
    if (cy >= vt->height) {
        cy = vt->height - 1;
    }

    if (vt->report_mouse && buttons != last_mask) {
        last_mask = buttons;
        if (buttons & 2) {
            mouse_report(vt, 0, cx, cy);
        }
        if (buttons & 4) {
            mouse_report(vt, 1, cx, cy);
        }
        if (buttons & 8) {
            mouse_report(vt, 2, cx, cy);
        }
        if (!buttons) {
            mouse_report(vt, 3, cx, cy);
        }
    }

    if (buttons & 4) {

        if (button2_released) {

            if (vdagent_owns_clipboard(vt)) {
                if (vt->selection) {
                    int i;
                    for (i = 0; i < vt->selection_len; i++) {
                        spiceterm_respond_unichar2(vt, vt->selection[i]);
                    }
                    spiceterm_update_watch_mask(vt, TRUE);
                    if (vt->y_displ != vt->y_base) {
                        vt->y_displ = vt->y_base;
                        spiceterm_refresh(vt);
                    }
                }
            } else {
                vdagent_request_clipboard(vt);
            }
        }

        button2_released = 0;
    } else {
        button2_released = 1;
    }

    if (buttons & 2) {
        int pos = cy * vt->width + cx;

        // code borrowed from libvncserver (VNCconsole.c)

        if (!vt->mark_active) {

            spiceterm_unselect_all(vt);

            vt->mark_active = 1;
            sel_start_pos = sel_end_pos = pos;
            spiceterm_toggle_marked_cell(vt, pos);

        } else {

            if (pos != sel_end_pos) {
                if (pos > sel_end_pos) {
                    cx = sel_end_pos;
                    cy = pos;
                } else {
                    cx = pos;
                    cy = sel_end_pos;
                }

                if (cx < sel_start_pos) {
                    if (cy < sel_start_pos) {
                        cy--;
                    }
                } else {
                    cx++;
                }

                while (cx <= cy) {
                    spiceterm_toggle_marked_cell(vt, cx);
                    cx++;
                }

                sel_end_pos = pos;
            }
        }

    } else if (vt->mark_active) {
        vt->mark_active = 0;

        if (sel_start_pos > sel_end_pos) {
            int tmp = sel_start_pos - 1;
            sel_start_pos = sel_end_pos;
            sel_end_pos = tmp;
        }

        int len = sel_end_pos - sel_start_pos + 1;

        if (vt->selection) {
            free(vt->selection);
        }
        vt->selection = (gunichar2 *)malloc(len * sizeof(gunichar2));
        vt->selection_len = len;

        for (i = 0; i < len; i++) {
            int pos = sel_start_pos + i;
            int x = pos % vt->width;
            int y1 = ((pos / vt->width) + vt->y_displ) % vt->total_height;
            TextCell *c = &vt->cells[y1 * vt->width + x];
            vt->selection[i] = c->ch;
            c++;
        }

        DPRINTF(1, "selection length = %d", vt->selection_len);

        vdagent_grab_clipboard(vt);
    }
}

void init_spiceterm(spiceTerm *vt, uint32_t width, uint32_t height) {
    int i;

    g_assert(vt != NULL);
    g_assert(vt->screen != NULL);

    vt->width = width / 8;
    vt->height = height / 16;

    vt->total_height = vt->height * 20;
    vt->scroll_height = 0;
    vt->y_base = 0;
    vt->y_displ = 0;

    vt->region_top = 0;
    vt->region_bottom = vt->height;

    vt->g0enc = LAT1_MAP;
    vt->g1enc = GRAF_MAP;
    vt->cur_enc = vt->g0enc;
    vt->charset = 0;

    /* default text attributes */
    vt->default_attrib.bold = 0;
    vt->default_attrib.uline = 0;
    vt->default_attrib.blink = 0;
    vt->default_attrib.invers = 0;
    vt->default_attrib.unvisible = 0;
    vt->default_attrib.fgcol = 7;
    vt->default_attrib.bgcol = 0;

    vt->cur_attrib = vt->default_attrib;

    if (vt->cells) {
        vt->cx = 0;
        vt->cy = 0;
        vt->cx_saved = 0;
        vt->cy_saved = 0;
        g_free(vt->cells);
    }

    vt->cells = (TextCell *)calloc(sizeof(TextCell), vt->width * vt->total_height);

    for (i = 0; i < vt->width * vt->total_height; i++) {
        vt->cells[i].ch = ' ';
        vt->cells[i].attrib = vt->default_attrib;
    }

    if (vt->altcells) {
        g_free(vt->altcells);
    }

    vt->altcells = (TextCell *)calloc(sizeof(TextCell), vt->width * vt->height);
}

void spiceterm_resize(spiceTerm *vt, uint32_t width, uint32_t height) {
    width = (width / 8) * 8;
    height = (height / 16) * 16;

    if (vt->screen->width == width && vt->screen->height == height) {
        return;
    }

    DPRINTF(0, "width=%u height=%u", width, height);

    spice_screen_resize(vt->screen, width, height);

    init_spiceterm(vt, width, height);

    struct winsize dimensions;
    dimensions.ws_col = vt->width;
    dimensions.ws_row = vt->height;

    ioctl(vt->pty, TIOCSWINSZ, &dimensions);
}

static gboolean master_error_callback(GIOChannel *channel, GIOCondition condition, gpointer data) {
    // spiceTerm *vt = (spiceTerm *)data;

    DPRINTF(1, "condition %d", condition);

    exit(0);

    return FALSE;
}

static void master_watch(int master, int event, void *opaque) {
    spiceTerm *vt = (spiceTerm *)opaque;
    int c;

    // fixme: if (!vt->mark_active) {

    if (event == SPICE_WATCH_EVENT_READ) {
        char buffer[1024];
        while ((c = read(master, buffer, 1024)) == -1) {
            if (errno != EAGAIN) {
                break;
            }
        }
        if (c == -1) {
            perror("master pipe read error"); // fixme
        }
        spiceterm_puts(vt, buffer, c);
    } else {
        if (vt->ibuf_count > 0) {
            DPRINTF(1, "write input %x %d", vt->ibuf[0], vt->ibuf_count);
            if ((c = write(master, vt->ibuf, vt->ibuf_count)) >= 0) {
                if (c == vt->ibuf_count) {
                    vt->ibuf_count = 0;
                } else if (c > 0) {
                    // not all data written
                    memmove(vt->ibuf, vt->ibuf + c, vt->ibuf_count - c);
                    vt->ibuf_count -= c;
                } else {
                    // nothing written -ignore and try later
                }
            } else {
                perror("master pipe write error");
            }
        }
        if (vt->ibuf_count == 0) {
            spiceterm_update_watch_mask(vt, FALSE);
        }
    }
}

static void spiceterm_print_usage(const char *msg) {
    if (msg) {
        fprintf(stderr, "ERROR: %s\n", msg);
    }
    fprintf(stderr, "USAGE: spiceterm [OPTIONS] [-- command [args]]\n");
    fprintf(
        stderr, "  --timeout <seconds>  Wait this time before aborting (default is 10 seconds)\n"
    );
    fprintf(stderr, "  --authpath <path>    Authentication path  (PVE AUTH)\n");
    fprintf(stderr, "  --permission <perm>  Required permissions (PVE AUTH)\n");
    fprintf(stderr, "  --port <port>        Bind to port <port>\n");
    fprintf(stderr, "  --addr <addr>        Bind to address <addr>\n");
    fprintf(stderr, "  --noauth             Disable authentication\n");
    fprintf(stderr, "  --keymap             Spefify keymap (uses kvm keymap files)\n");
}

int main(int argc, char **argv) {
    int c;
    char **cmdargv = NULL;
    char *command = "/bin/bash"; // execute normal shell as default
    int pid;
    int master;
    char ptyname[1024];
    struct winsize dimensions;
    SpiceTermOptions opts = {
        .timeout = 10,
        .port = 5900,
        .addr = NULL,
        .noauth = FALSE,
    };

    static struct option long_options[] = {
        {"timeout", required_argument, 0, 't'},
        {"authpath", required_argument, 0, 'A'},
        {"permissions", required_argument, 0, 'P'},
        {"port", required_argument, 0, 'p'},
        {"addr", required_argument, 0, 'a'},
        {"keymap", required_argument, 0, 'k'},
        {"noauth", no_argument, 0, 'n'},
        {NULL, 0, 0, 0},
    };

    while ((c = getopt_long(argc, argv, "nkt:a:p:P:", long_options, NULL)) != -1) {
        switch (c) {
        case 'n':
            opts.noauth = TRUE;
            break;
        case 'k':
            opts.keymap = optarg;
            break;
        case 'A':
            pve_auth_set_path(optarg);
            break;
        case 'P':
            pve_auth_set_permissions(optarg);
            break;
        case 'p':
            opts.port = atoi(optarg);
            break;
        case 'a':
            opts.addr = optarg;
            break;
        case 't':
            opts.timeout = atoi(optarg);
            break;
        case '?':
            spiceterm_print_usage(NULL);
            exit(-1);
            break;
        default:
            spiceterm_print_usage("getopt returned unknown character code");
            exit(-1);
        }
    }

    if (optind < argc) {
        command = argv[optind];
        cmdargv = &argv[optind];
    }

    spiceTerm *vt = spiceterm_create(744, 400, &opts);
    if (!vt) {
        exit(-1);
    }

    setlocale(LC_ALL, ""); // set from environment

    char *ctype = setlocale(LC_CTYPE, NULL); // query LC_CTYPE

    // fixme: ist there a standard way to detect utf8 mode ?
    if (strcasestr(ctype, ".utf-8") || strcasestr(ctype, ".utf8")) {
        vt->utf8 = 1;
    }

    dimensions.ws_col = vt->width;
    dimensions.ws_row = vt->height;

    setenv("TERM", TERM, 1);

    DPRINTF(1, "execute %s", command);

    pid = forkpty(&master, ptyname, NULL, &dimensions);
    if (!pid) {

        // install default signal handlers
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);

        if (cmdargv) {
            execvp(command, cmdargv);
        } else {
            execlp(command, command, NULL);
        }
        perror("Error: exec failed\n");
        exit(-1); // should not be reached
    } else if (pid == -1) {
        perror("Error: fork failed\n");
        exit(-1);
    }

    vt->pty = master;

    /* watch for errors - we need to use glib directly because spice
     * does not have SPICE_WATCH_EVENT for this */
    GIOChannel *channel = g_io_channel_unix_new(master);
    g_io_channel_set_flags(channel, G_IO_FLAG_NONBLOCK, NULL);
    g_io_channel_set_encoding(channel, NULL, NULL);
    g_io_add_watch(channel, G_IO_ERR | G_IO_HUP, master_error_callback, vt);

    vt->screen->mwatch = vt->screen->core->watch_add(
        master, SPICE_WATCH_EVENT_READ /* |SPICE_WATCH_EVENT_WRITE */, master_watch, vt
    );

    basic_event_loop_mainloop();

    kill(pid, 9);
    int status;
    waitpid(pid, &status, 0);

    exit(0);
}
