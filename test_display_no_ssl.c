/**
 * Test ground for developing specific tests.
 *
 * Any specific test can start of from here and set the server to the
 * specific required state, and create specific operations or reuse
 * existing ones in the test_display_base supplied queue.
 */

#include <stdlib.h>
#include <stdio.h>
#include "test_display_base.h"

SpiceCoreInterface *core;
SpiceTimer *ping_timer;

int ping_ms = 1000;

void pinger(void *opaque)
{
    printf("TEST PINGER\n");

    core->timer_start(ping_timer, ping_ms);
}

int main(void)
{
    Test *test;

    core = basic_event_loop_init();
    test = test_new(core);
    //spice_server_set_image_compression(server, SPICE_IMAGE_COMPRESS_OFF);
    test_add_display_interface(test);
 
    test_add_agent_interface(test->server);

    test_add_keyboard_interface(test);
    
    //ping_timer = core->timer_add(pinger, NULL);
    //core->timer_start(ping_timer, ping_ms);

    basic_event_loop_mainloop();

    return 0;
}
