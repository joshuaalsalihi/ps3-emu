// Threads sharing data through PSL1GHT's synchronization primitives. Only the main thread
// prints, after its workers finish, so the output doesn't depend on how threads interleave.

#include <ppu-lv2.h> // must come before the sys/ headers
#include <stdio.h>
#include <sys/cond.h>
#include <sys/mutex.h>
#include <sys/sem.h>
#include <sys/systime.h>
#include <sys/thread.h>
#include <unistd.h>

enum { k_workers = 4, k_queue_size = 4, k_items = 100, k_rounds = 10 };

static sys_ppu_thread_t start(void (*entry)(void*), void* arg) {
    sys_ppu_thread_t id;
    sysThreadCreate(&id, entry, arg, 1000, 0x4000, THREAD_JOINABLE, "worker");
    return id;
}

static u64 join(sys_ppu_thread_t id) {
    u64 exit_value = 0;
    sysThreadJoin(id, &exit_value);
    return exit_value;
}

// ---- Several threads adding to one total under a lightweight mutex ----

static sys_lwmutex_t total_lock;
static u64 total;

static void add_numbers(void* arg) {
    (void)arg;
    for (u64 n = 1; n <= 1000; n++) {
        sysLwMutexLock(&total_lock, 0);
        total += n;
        sysLwMutexUnlock(&total_lock);
        if (n % 100 == 0) sysThreadYield();
    }
    sysThreadExit(0);
}

// ---- A producer and a consumer sharing a small queue ----

static sys_lwmutex_t queue_lock;
static sys_lwcond_t queue_not_empty, queue_not_full;
static int queue[k_queue_size];
static int queue_count, queue_head;

static void produce(void* arg) {
    (void)arg;
    for (int item = 1; item <= k_items; item++) {
        sysLwMutexLock(&queue_lock, 0);
        while (queue_count == k_queue_size) {
            sysLwCondWait(&queue_not_full, 0);
        }
        queue[(queue_head + queue_count) % k_queue_size] = item;
        queue_count++;
        sysLwCondSignal(&queue_not_empty);
        sysLwMutexUnlock(&queue_lock);
    }
    sysThreadExit(0);
}

static void consume(void* arg) {
    (void)arg;
    u64 sum = 0;
    for (int i = 0; i < k_items; i++) {
        sysLwMutexLock(&queue_lock, 0);
        while (queue_count == 0) {
            sysLwCondWait(&queue_not_empty, 0);
        }
        sum += queue[queue_head];
        queue_head = (queue_head + 1) % k_queue_size;
        queue_count--;
        sysLwCondSignal(&queue_not_full);
        sysLwMutexUnlock(&queue_lock);
    }
    sysThreadExit(sum); // the exit value carries the result back to join
}

// ---- Two threads taking turns with semaphores ----

static sys_sem_t ping, pong;
static int turns;

static void ponger(void* arg) {
    (void)arg;
    for (int i = 0; i < k_rounds; i++) {
        sysSemWait(ping, 0);
        turns++;
        sysSemPost(pong, 1);
    }
    sysThreadExit(0);
}

int main(void) {
    sys_lwmutex_attr_t mutex_attr = {SYS_LWMUTEX_PROTOCOL_FIFO, SYS_LWMUTEX_ATTR_NOT_RECURSIVE, ""};
    sys_lwcond_attr_t cond_attr = {""};
    sys_sem_attr_t sem_attr = {SYS_SEM_ATTR_PROTOCOL, SYS_SEM_ATTR_PSHARED, 0, 0, 0, ""};

    // Shared total
    sysLwMutexCreate(&total_lock, &mutex_attr);
    sys_ppu_thread_t workers[k_workers];
    for (int i = 0; i < k_workers; i++) {
        workers[i] = start(add_numbers, NULL);
    }
    for (int i = 0; i < k_workers; i++) {
        join(workers[i]);
    }
    printf("%d threads added 1..1000: total %llu\n", k_workers, (unsigned long long)total);

    // Producer and consumer
    sysLwMutexCreate(&queue_lock, &mutex_attr);
    sysLwCondCreate(&queue_not_empty, &queue_lock, &cond_attr);
    sysLwCondCreate(&queue_not_full, &queue_lock, &cond_attr);
    sys_ppu_thread_t producer = start(produce, NULL);
    sys_ppu_thread_t consumer = start(consume, NULL);
    join(producer);
    printf("consumer received 1..%d: sum %llu\n", k_items, (unsigned long long)join(consumer));

    // Ping-pong
    sysSemCreate(&ping, &sem_attr, 0, 1);
    sysSemCreate(&pong, &sem_attr, 0, 1);
    sys_ppu_thread_t partner = start(ponger, NULL);
    for (int i = 0; i < k_rounds; i++) {
        sysSemPost(ping, 1);
        sysSemWait(pong, 0);
    }
    join(partner);
    printf("ping-pong: %d turns\n", turns);

    // Sleeping advances the (virtual) clock
    const u64 before = sysGetSystemTime();
    usleep(5000);
    const u64 slept = sysGetSystemTime() - before;
    printf("slept at least 5 ms: %s\n", slept >= 5000 ? "yes" : "no");
    return 0;
}
