// The SPU side of spu_threads.c. Each worker gets a multiplier from its mailbox, sums its half of
// the input by DMA, puts the scaled sum back, and increments the shared counter atomically.

#include <spu_mfcio.h>
#include <sys/spu_thread.h>

#include "spu_threads.h"

#define TAG 1
#define HALF (COUNT / WORKERS)

static unsigned int input[HALF] __attribute__((aligned(128)));
static unsigned int result[4] __attribute__((aligned(16)));
static unsigned int line[32] __attribute__((aligned(128)));

static void wait_for_dma(void) {
    mfc_write_tag_mask(1 << TAG);
    mfc_read_tag_status_all();
}

// The classic lock-line increment: load and reserve, change, store if still reserved.
static void atomic_increment(unsigned long long counter) {
    do {
        mfc_getllar(line, counter, 0, 0);
        mfc_read_atomic_status();
        line[0] += 1;
        mfc_putllc(line, counter, 0, 0);
    } while (mfc_read_atomic_status() & MFC_PUTLLC_STATUS);
}

int main(unsigned long long shared, unsigned long long index) {
    const unsigned int multiplier = spu_read_in_mbox();

    mfc_get(input, shared + INPUT_OFFSET + index * sizeof(input), sizeof(input), TAG, 0, 0);
    wait_for_dma();

    unsigned int sum = 0;
    for (int i = 0; i < HALF; ++i) {
        sum += input[i];
    }
    result[0] = sum * multiplier;
    mfc_put(result, shared + RESULTS_OFFSET + index * sizeof(result), sizeof(result), TAG, 0, 0);

    for (int i = 0; i < INCREMENTS; ++i) {
        atomic_increment(shared + COUNTER_OFFSET);
    }

    wait_for_dma(); // the put must land before the PPU looks
    spu_thread_exit(100 + (int)index);
    return 0;
}
