#ifndef A7C2_STREAM_SLOTS_H
#define A7C2_STREAM_SLOTS_H

#include <stdatomic.h>
#include <stdint.h>

/* Each sequence is published by the reader and acquired by the consumer.
 * Only the consumer recycles its slot. The owner joins both roles after stop
 * before reusing the control block or storage. Waiting belongs to the caller. */
typedef struct {
    _Atomic(uint32_t) tag[2];
    _Atomic(uint32_t) stopped;
} StreamSlots;

static inline void slots_init(StreamSlots *slots) {
    atomic_init(&slots->tag[0], 0);
    atomic_init(&slots->tag[1], 1);
    atomic_init(&slots->stopped, 0);
}

static inline int slots_status(const StreamSlots *slots, uint32_t sequence, uint32_t offset) {
    if (atomic_load_explicit(&slots->stopped, memory_order_acquire)) return -1;
    return atomic_load_explicit(&slots->tag[sequence & 1], memory_order_acquire) == sequence + offset;
}

static inline int slots_publish(StreamSlots *slots, uint32_t sequence) {
    int free = slots_status(slots, sequence, 0);
    if (free == 1) atomic_store_explicit(&slots->tag[sequence & 1], sequence + 1, memory_order_release);
    return free;
}

static inline int slots_release(StreamSlots *slots, uint32_t sequence) {
    if (atomic_load_explicit(&slots->tag[sequence & 1], memory_order_relaxed) != sequence + 1) return 0;
    atomic_store_explicit(&slots->tag[sequence & 1], sequence + 2, memory_order_release);
    return 1;
}

static inline void slots_stop(StreamSlots *slots) {
    atomic_store_explicit(&slots->stopped, 1, memory_order_release);
}

#endif
