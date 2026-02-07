// Roll No: MT25082
// =============================================================================
// File:    MT25082_common.h
// Purpose: Common header for PA02 — Network I/O primitives analysis.
//          Declares shared data structures and utility function prototypes
//          used across all three socket implementations (two-copy, one-copy,
//          zero-copy).
// =============================================================================

#ifndef MT25082_COMMON_H
#define MT25082_COMMON_H

// ---------------------------------------------------------------------------
//  Standard C headers
// ---------------------------------------------------------------------------
#include <stdio.h>              /* printf, fprintf, perror                   */
#include <stdlib.h>             /* malloc, free, exit, atoi, atol            */
#include <string.h>             /* memset, memcpy, strlen                    */
#include <errno.h>              /* errno, strerror                           */
#include <unistd.h>             /* close, read, write                        */

// ---------------------------------------------------------------------------
//  POSIX threading
// ---------------------------------------------------------------------------
#include <pthread.h>            /* pthread_create, pthread_join, mutex, etc. */

// ---------------------------------------------------------------------------
//  Networking / sockets
// ---------------------------------------------------------------------------
#include <sys/socket.h>         /* socket, bind, listen, accept, send, recv  */
#include <sys/types.h>          /* ssize_t, size_t, socklen_t                */
#include <netinet/in.h>         /* sockaddr_in, htons, htonl                 */
#include <netinet/tcp.h>        /* TCP_NODELAY                               */
#include <arpa/inet.h>          /* inet_pton, inet_ntoa                      */

// ---------------------------------------------------------------------------
//  Timing (high-resolution, perf-friendly)
// ---------------------------------------------------------------------------
#include <time.h>               /* clock_gettime, struct timespec            */
#include <sys/time.h>           /* gettimeofday (fallback)                   */

// ---------------------------------------------------------------------------
//  Zero-copy / sendmsg support
// ---------------------------------------------------------------------------
#include <sys/uio.h>            /* struct iovec, writev, readv               */

// ---------------------------------------------------------------------------
//  Signal handling (graceful shutdown)
// ---------------------------------------------------------------------------
#include <signal.h>             /* signal, SIGPIPE                           */

// ---------------------------------------------------------------------------
//  Boolean convenience (C99+)
// ---------------------------------------------------------------------------
#include <stdbool.h>            /* bool, true, false                         */

// ===========================================================================
//  Constants
// ===========================================================================

#define NUM_FIELDS         8    /* Number of dynamically allocated string     */
                               /* fields inside message_t.                   */

#define DEFAULT_PORT       9090 /* Default TCP port for client–server comms.  */

#define DEFAULT_DURATION   10   /* Default experiment duration in seconds.    */

#define BACKLOG            64   /* listen() backlog queue size.               */

// ===========================================================================
//  Data Structures
// ===========================================================================

// ---------------------------------------------------------------------------
//  message_t
//  ---------
//  Represents a single network message composed of exactly 8 dynamically
//  allocated string fields.  Each field points to a heap buffer whose size
//  is determined at runtime (msg_size / NUM_FIELDS bytes per field).
//
//  Memory layout (after allocation):
//      field[0] -> malloc'd buffer of (msg_size / 8) bytes
//      field[1] -> malloc'd buffer of (msg_size / 8) bytes
//      ...
//      field[7] -> malloc'd buffer of (msg_size / 8) bytes
// ---------------------------------------------------------------------------
typedef struct {
    char *field[NUM_FIELDS];    /* 8 dynamically allocated string buffers    */
} message_t;

// ---------------------------------------------------------------------------
//  thread_args_t
//  -------------
//  Encapsulates the per-thread context passed to each client / server
//  worker thread.
//
//  Members:
//      sock_fd      – connected socket file descriptor
//      msg_size     – total message size in bytes (split across 8 fields)
//      duration_sec – how long (seconds) the thread should keep sending
// ---------------------------------------------------------------------------
typedef struct {
    int    sock_fd;             /* Connected socket file descriptor          */
    size_t msg_size;            /* Total message payload size (bytes)        */
    int    duration_sec;        /* Duration of continuous transfer (seconds) */
} thread_args_t;

// ===========================================================================
//  Utility Function Declarations
// ===========================================================================

// ---------------------------------------------------------------------------
//  allocate_message
//  ----------------
//  Allocates heap memory for each of the 8 string fields inside *msg.
//  Each field receives (msg_size / NUM_FIELDS) bytes.
//
//  Parameters:
//      msg      – pointer to an existing message_t whose fields will be
//                 allocated
//      msg_size – total desired message payload size in bytes
// ---------------------------------------------------------------------------
void allocate_message(message_t *msg, size_t msg_size);

// ---------------------------------------------------------------------------
//  free_message
//  ------------
//  Frees every dynamically allocated field inside *msg and sets the
//  pointers to NULL to prevent dangling references.
//
//  Parameters:
//      msg – pointer to a message_t whose fields should be freed
// ---------------------------------------------------------------------------
void free_message(message_t *msg);

// ---------------------------------------------------------------------------
//  fill_message
//  ------------
//  Populates all 8 fields of *msg with synthetic payload data (e.g., a
//  repeating character pattern).  Useful for generating deterministic
//  content before sending.
//
//  Parameters:
//      msg      – pointer to a previously allocated message_t
//      msg_size – total message size (each field gets msg_size / 8 bytes)
// ---------------------------------------------------------------------------
void fill_message(message_t *msg, size_t msg_size);

// ---------------------------------------------------------------------------
//  get_time_us
//  -----------
//  Returns the current wall-clock time in microseconds (µs) using
//  CLOCK_MONOTONIC for perf-friendly, high-resolution timing that is
//  immune to NTP adjustments.
//
//  Returns:
//      Current time as a double, in microseconds.
// ---------------------------------------------------------------------------
double get_time_us(void);

#endif /* MT25082_COMMON_H */
