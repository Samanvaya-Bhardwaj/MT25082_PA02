// Roll No: MT25082
// =============================================================================
// File:    MT25082_common.c
// Purpose: Implements the shared utility functions declared in
//          MT25082_common.h.  These helpers handle message allocation,
//          population, deallocation, and high-resolution timing for the
//          three socket communication experiments (two-copy, one-copy,
//          zero-copy).
//
// Design notes:
//   • Every message field is individually heap-allocated with malloc().
//     Heap allocation is used (rather than stack or static buffers) because:
//       1. Message sizes are determined at runtime (parameterized).
//       2. Each thread gets its own independent buffers, avoiding shared
//          mutable state and making the code inherently thread-safe.
//       3. Large messages would overflow the stack; heap has no such limit.
//       4. Mirrors the real-world pattern where application data resides
//          in dynamically allocated memory before being handed to the
//          kernel for network transmission.
//   • All functions operate exclusively on their arguments — no global
//     variables are read or written — so concurrent calls from multiple
//     threads are safe without locks.
// =============================================================================

#include "MT25082_common.h"

// ===========================================================================
//  allocate_message
// ===========================================================================
//  Allocates 8 separate heap buffers for the fields of *msg.
//
//  Each buffer receives (msg_size / NUM_FIELDS) bytes.  If msg_size is not
//  evenly divisible by 8, the last field absorbs the remainder so that the
//  total allocated memory equals msg_size exactly.
//
//  Why heap allocation?
//  --------------------
//  • The message size is only known at runtime (user parameter), so we
//    cannot use fixed-size stack arrays.
//  • Heap memory gives each thread its own private copy, which is
//    essential for thread safety without synchronisation overhead.
//  • The kernel's sendmsg / send path copies from user-space buffers;
//    having them on the heap faithfully represents the data-movement
//    cost we want to measure.
// ---------------------------------------------------------------------------
void allocate_message(message_t *msg, size_t msg_size)
{
    /* Guard against NULL pointer or zero-size request */
    if (msg == NULL || msg_size == 0) {
        fprintf(stderr, "[allocate_message] ERROR: NULL msg or zero size\n");
        return;
    }

    /* Base size for each of the 8 fields */
    size_t per_field = msg_size / NUM_FIELDS;

    /* Remainder bytes that don't divide evenly into 8 */
    size_t remainder = msg_size % NUM_FIELDS;

    for (int i = 0; i < NUM_FIELDS; i++) {
        /*
         * The last field picks up any leftover bytes so the total
         * allocation sums to exactly msg_size.
         */
        size_t alloc_size = per_field + ((i == NUM_FIELDS - 1) ? remainder : 0);

        msg->field[i] = (char *)malloc(alloc_size);

        if (msg->field[i] == NULL) {
            perror("[allocate_message] malloc failed");

            /* Roll back any fields already allocated to prevent leaks */
            for (int j = 0; j < i; j++) {
                free(msg->field[j]);
                msg->field[j] = NULL;
            }
            return;
        }

        /* Zero-initialise to avoid valgrind/undefined-behaviour warnings */
        memset(msg->field[i], 0, alloc_size);
    }
}

// ===========================================================================
//  fill_message
// ===========================================================================
//  Populates every field of *msg with deterministic, repeating ASCII data.
//
//  Field i is filled with the character 'A' + (i % 26), producing the
//  pattern:  field[0] = "AAAA…", field[1] = "BBBB…", … field[7] = "HHHH…".
//
//  This deterministic content makes it easy to verify correctness on the
//  receiving side and ensures reproducible cache / memory-access patterns
//  across experiment runs.
// ---------------------------------------------------------------------------
void fill_message(message_t *msg, size_t msg_size)
{
    if (msg == NULL || msg_size == 0) {
        return;
    }

    size_t per_field = msg_size / NUM_FIELDS;
    size_t remainder = msg_size % NUM_FIELDS;

    for (int i = 0; i < NUM_FIELDS; i++) {
        /* Skip fields that were never allocated */
        if (msg->field[i] == NULL) {
            continue;
        }

        /* Determine the actual size of this field's buffer */
        size_t field_size = per_field + ((i == NUM_FIELDS - 1) ? remainder : 0);

        /*
         * Fill with a repeating character unique to this field index.
         * memset is used here for speed — it is typically implemented
         * with optimised SIMD instructions on modern platforms.
         */
        char fill_char = 'A' + (i % 26);
        memset(msg->field[i], fill_char, field_size);
    }
}

// ===========================================================================
//  free_message
// ===========================================================================
//  Safely frees all 8 heap-allocated field buffers in *msg.
//
//  After freeing, each pointer is set to NULL.  This prevents:
//    • Double-free errors if free_message() is accidentally called twice.
//    • Dangling-pointer dereferences in subsequent code.
// ---------------------------------------------------------------------------
void free_message(message_t *msg)
{
    if (msg == NULL) {
        return;
    }

    for (int i = 0; i < NUM_FIELDS; i++) {
        if (msg->field[i] != NULL) {
            free(msg->field[i]);
            msg->field[i] = NULL;   /* Prevent dangling pointer / double-free */
        }
    }
}

// ===========================================================================
//  get_time_us
// ===========================================================================
//  Returns the current wall-clock time in microseconds (µs).
//
//  Uses CLOCK_MONOTONIC, which:
//    • Is immune to NTP adjustments and manual clock changes.
//    • Provides nanosecond-level granularity on modern Linux kernels.
//    • Is the recommended clock source for benchmarking / profiling.
//
//  The result is a double so it can represent sub-microsecond fractions
//  without integer truncation, while remaining convenient for arithmetic
//  (e.g., elapsed = end - start).
//
//  Thread safety: clock_gettime() is reentrant and safe to call from
//  multiple threads concurrently without any locking.
// ---------------------------------------------------------------------------
double get_time_us(void)
{
    struct timespec ts;

    /* Retrieve monotonic clock — should never fail on Linux */
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("[get_time_us] clock_gettime failed");
        return 0.0;
    }

    /*
     * Convert seconds + nanoseconds into a single microsecond value:
     *   µs = seconds * 1,000,000  +  nanoseconds / 1,000
     */
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}
