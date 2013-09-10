#ifndef __TEST_DISPLAY_BASE_H__
#define __TEST_DISPLAY_BASE_H__

#include <glib.h>
#include <spice.h>

#include "basic_event_loop.h"


typedef struct TextAttributes {
  unsigned int fgcol:4;
  unsigned int bgcol:4;
  unsigned int bold:1;
  unsigned int uline:1;
  unsigned int blink:1;
  unsigned int invers:1;
  unsigned int unvisible:1;
} TextAttributes;

#define COUNT(x) ((sizeof(x)/sizeof(x[0])))

typedef struct Test Test;

#define COMMANDS_SIZE 1024

#define MAX_HEIGHT 2048
#define MAX_WIDTH 2048

struct Test {
    SpiceCoreInterface *core;
    SpiceServer *server;

    QXLInstance qxl_instance;
    QXLWorker *qxl_worker;

    SpiceKbdInstance keyboard_sin;

    uint8_t primary_surface[MAX_HEIGHT * MAX_WIDTH * 4];
    int primary_height;
    int primary_width;

    SpiceTimer *conn_timeout_timer;
    SpiceWatch *mwatch; /* watch master pty */

    int cursor_notify;

    // Current mode (set by create_primary)
    int width;
    int height;

    int target_surface;

    GCond* command_cond;
    GMutex* command_mutex;

    int commands_end;
    int commands_start;
    struct QXLCommandExt* commands[COMMANDS_SIZE];

    // callbacks
    void (*on_client_connected)(Test *test);
    void (*on_client_disconnected)(Test *test);
};

void test_add_display_interface(Test *test);
void test_add_agent_interface(SpiceServer *server); // TODO - Test *test
void test_add_keyboard_interface(Test *test);
Test* test_new(SpiceCoreInterface* core);

void test_draw_update_char(Test *test, int x, int y, gunichar ch, TextAttributes attrib);
void test_spice_scroll(Test *test, int x1, int y1, int x2, int y2, int src_x, int src_y);
void test_spice_clear(Test *test, int x1, int y1, int x2, int y2);


uint32_t test_get_width(void);
uint32_t test_get_height(void);

void spice_test_config_parse_args(int argc, char **argv);

#endif /* __TEST_DISPLAY_BASE_H__ */
