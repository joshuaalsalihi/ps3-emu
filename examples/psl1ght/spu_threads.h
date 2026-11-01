// Shared by spu_threads.c (PPU) and spu_threads_spu.c (SPU): the data the SPUs reach by DMA.

#define WORKERS 2
#define COUNT 64 // input values, split evenly between the workers
#define INCREMENTS 1000

// Each part sits on its own 128-byte line, the unit of DMA atomics, so updating the counter
// never disturbs the rest.
struct shared {
    unsigned int input[COUNT] __attribute__((aligned(128)));
    unsigned int results[WORKERS][4] __attribute__((aligned(128))); // one quadword per worker
    unsigned int counter __attribute__((aligned(128)));
} __attribute__((aligned(128)));

#define INPUT_OFFSET 0
#define RESULTS_OFFSET 256
#define COUNTER_OFFSET 384
