// SPU events: the PPU sends commands to an SPU thread (spu_events_spu.c) through an event queue
// bound to it, and receives the results as events from one of the thread's ports. The SPU prints
// with spu_printf, which is itself built on events: PSL1GHT's handler thread on the PPU receives
// each request, formats it from the SPU's local store, and replies through the mailbox.

#include <lv2/spu.h>
#include <stdio.h>
#include <sys/event_queue.h>
#include <sys/spu.h>

#include "spu_events.h"

// The SPU program, embedded by bin2s.
extern const u8 spu_events_spu_elf[];

static void check(const char* what, s32 result) {
    if (result != 0) printf("%s failed: %#x\n", what, (unsigned int)result);
}

static sys_event_queue_t create_queue(s32 type, const char* name) {
    sys_event_queue_attr_t attributes = {SYS_EVENT_QUEUE_FIFO, type, ""};
    snprintf(attributes.name, sizeof(attributes.name), "%s", name);
    sys_event_queue_t queue = 0;
    check("sysEventQueueCreate",
          sysEventQueueCreate(&queue, &attributes, SYS_EVENT_QUEUE_KEY_LOCAL, 8));
    return queue;
}

int main(void) {
    const u32 commands[][3] = {{2, 3, 4}, {10, 20, 30}, {7, 7, 7}};
    const int command_count = sizeof(commands) / sizeof(commands[0]);

    sysSpuImage image;
    sys_spu_group_t group;
    sys_spu_thread_t thread;

    check("sysSpuInitialize", sysSpuInitialize(6, 0));
    check("sysSpuPrintfInitialize", sysSpuPrintfInitialize(1000, NULL));
    check("sysSpuImageImport", sysSpuImageImport(&image, spu_events_spu_elf, SPU_IMAGE_PROTECT));

    sysSpuThreadGroupAttribute group_attributes;
    sysSpuThreadGroupAttributeInitialize(group_attributes);
    sysSpuThreadGroupAttributeName(group_attributes, "events");
    check("sysSpuThreadGroupCreate", sysSpuThreadGroupCreate(&group, 1, 100, &group_attributes));

    sysSpuThreadAttribute attributes;
    sysSpuThreadAttributeInitialize(attributes);
    sysSpuThreadAttributeName(attributes, "calculator");
    sysSpuThreadArgument arguments;
    sysSpuThreadArgumentInitialize(arguments);
    check("sysSpuThreadInitialize",
          sysSpuThreadInitialize(&thread, group, 0, &image, &attributes, &arguments));
    check("sysSpuPrintfAttachThread", sysSpuPrintfAttachThread(thread));

    // Results: the SPU thread's port RESULT_PORT sends to a PPU queue.
    const sys_event_queue_t results = create_queue(SYS_EVENT_QUEUE_PPU, "results");
    check("sysSpuThreadConnectEvent",
          sysSpuThreadConnectEvent(thread, results, SPU_THREAD_EVENT_USER, RESULT_PORT));

    // Commands: a PPU port sends to an SPU queue bound to the thread.
    const sys_event_queue_t queue = create_queue(SYS_EVENT_QUEUE_SPU, "commands");
    check("sysSpuThreadBindQueue", sysSpuThreadBindQueue(thread, queue, COMMAND_QUEUE));
    sys_event_port_t port;
    check("sysEventPortCreate",
          sysEventPortCreate(&port, SYS_EVENT_PORT_LOCAL, SYS_EVENT_PORT_NO_NAME));
    check("sysEventPortConnectLocal", sysEventPortConnectLocal(port, queue));

    check("sysSpuThreadGroupStart", sysSpuThreadGroupStart(group));
    for (int i = 0; i < command_count; ++i) {
        check("sysEventPortSend",
              sysEventPortSend(port, commands[i][0], commands[i][1], commands[i][2]));
        sys_event_t event;
        check("sysEventQueueReceive", sysEventQueueReceive(results, &event, 0));
        printf("ppu: %s event from %s, port %u: %u -> %u\n",
               event.source == SPU_THREAD_EVENT_USER_KEY ? "user" : "unknown",
               event.data_1 == thread ? "the calculator" : "someone else",
               (unsigned int)(event.data_2 >> 32), (unsigned int)event.data_2,
               (unsigned int)event.data_3);
    }
    check("sysEventPortSend", sysEventPortSend(port, 0, 0, 0)); // stop

    u32 cause = 0, status = 0;
    check("sysSpuThreadGroupJoin", sysSpuThreadGroupJoin(group, &cause, &status));
    s32 exit_status = 0;
    check("sysSpuThreadGetExitStatus", sysSpuThreadGetExitStatus(thread, &exit_status));
    printf("group ended: cause %u, commands handled %d\n", cause, exit_status);

    check("sysEventPortDisconnect", sysEventPortDisconnect(port));
    check("sysEventPortDestroy", sysEventPortDestroy(port));
    check("sysSpuThreadUnbindQueue", sysSpuThreadUnbindQueue(thread, COMMAND_QUEUE));
    check("sysSpuThreadDisconnectEvent",
          sysSpuThreadDisconnectEvent(thread, SPU_THREAD_EVENT_USER, RESULT_PORT));
    check("sysSpuPrintfDetachThread", sysSpuPrintfDetachThread(thread));
    check("sysEventQueueDestroy", sysEventQueueDestroy(queue, 0));
    check("sysEventQueueDestroy", sysEventQueueDestroy(results, 0));
    check("sysSpuPrintfFinalize", sysSpuPrintfFinalize());
    check("sysSpuThreadGroupDestroy", sysSpuThreadGroupDestroy(group));
    check("sysSpuImageClose", sysSpuImageClose(&image));
    return 0;
}
