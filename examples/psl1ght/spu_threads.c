// SPU threads: two workers (spu_threads_spu.c) sum halves of an array by DMA, while all three
// threads increment one counter atomically, the PPU with lwarx/stwcx. and the SPUs with
// getllar/putllc. The counter ends at 3 x INCREMENTS only if the atomics really are atomic.

#include <stddef.h>
#include <stdio.h>
#include <sys/atomic.h>
#include <sys/spu.h>

#include "spu_threads.h"

_Static_assert(offsetof(struct shared, results) == RESULTS_OFFSET, "layout");
_Static_assert(offsetof(struct shared, counter) == COUNTER_OFFSET, "layout");

// The SPU program, embedded by bin2s.
extern const u8 spu_threads_spu_elf[];

static struct shared shared;

static void check(const char* what, s32 result) {
    if (result != 0) printf("%s failed: %#x\n", what, (unsigned int)result);
}

int main(void) {
    for (int i = 0; i < COUNT; ++i) {
        shared.input[i] = i * i + 1;
    }
    const u32 multipliers[WORKERS] = {3, 5};

    sysSpuImage image;
    sys_spu_group_t group;
    sys_spu_thread_t threads[WORKERS];

    check("sysSpuInitialize", sysSpuInitialize(6, 0));
    check("sysSpuImageImport", sysSpuImageImport(&image, spu_threads_spu_elf, SPU_IMAGE_PROTECT));

    sysSpuThreadGroupAttribute group_attributes;
    sysSpuThreadGroupAttributeInitialize(group_attributes);
    sysSpuThreadGroupAttributeName(group_attributes, "workers");
    check("sysSpuThreadGroupCreate",
          sysSpuThreadGroupCreate(&group, WORKERS, 100, &group_attributes));

    for (u32 i = 0; i < WORKERS; ++i) {
        sysSpuThreadAttribute attributes;
        sysSpuThreadAttributeInitialize(attributes);
        sysSpuThreadAttributeName(attributes, "worker");
        sysSpuThreadArgument arguments;
        sysSpuThreadArgumentInitialize(arguments);
        arguments.arg0 = (u64)&shared;
        arguments.arg1 = i;
        check("sysSpuThreadInitialize",
              sysSpuThreadInitialize(&threads[i], group, i, &image, &attributes, &arguments));
    }

    check("sysSpuThreadGroupStart", sysSpuThreadGroupStart(group));
    for (u32 i = 0; i < WORKERS; ++i) {
        check("sysSpuThreadWriteMb", sysSpuThreadWriteMb(threads[i], multipliers[i]));
    }
    for (int i = 0; i < INCREMENTS; ++i) {
        sysAtomicInc((atomic_t*)&shared.counter);
    }

    u32 cause = 0, status = 0;
    check("sysSpuThreadGroupJoin", sysSpuThreadGroupJoin(group, &cause, &status));
    printf("group ended: cause %u\n", cause);
    for (u32 i = 0; i < WORKERS; ++i) {
        s32 exit_status = 0;
        check("sysSpuThreadGetExitStatus", sysSpuThreadGetExitStatus(threads[i], &exit_status));
        printf("spu %u: exit status %d, sum x %u = %u\n", i, exit_status, multipliers[i],
               shared.results[i][0]);
    }
    printf("counter: %u\n", shared.counter);

    check("sysSpuThreadGroupDestroy", sysSpuThreadGroupDestroy(group));
    check("sysSpuImageClose", sysSpuImageClose(&image));
    return 0;
}
