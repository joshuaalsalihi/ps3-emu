// The SPU side of spu_events.c: receives commands {a, b, c} from an event queue, prints each
// with spu_printf, and sends a * b + c back as an event. A command with a = 0 ends it.

#include <stdint.h>
#include <sys/spu_event.h>
#include <sys/spu_printf.h>
#include <sys/spu_thread.h>

#include "spu_events.h"

int main(void) {
    spu_printf("spu: waiting for commands\n");
    unsigned int handled = 0;
    for (;;) {
        uint32_t a, b, c;
        if (spu_thread_receive_event(COMMAND_QUEUE, &a, &b, &c) != 0) spu_thread_exit(-1);
        if (a == 0) break;

        const uint32_t value = a * b + c;
        spu_printf("spu: %u x %u + %u = %u\n", a, b, c, value);
        if (spu_thread_send_event(RESULT_PORT, a, value) != 0) spu_thread_exit(-2);
        ++handled;
    }
    spu_thread_exit((int)handled);
    return 0;
}
