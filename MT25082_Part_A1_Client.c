// Roll No: MT25082
// =============================================================================
// File:    MT25082_Part_A1_Client.c
// Purpose: Two-Copy Baseline TCP Client
//
//          This client connects to the A1 server using standard recv().
//          It spawns a configurable number of threads, each opening its own
//          TCP connection.  Every thread receives data continuously for a
//          fixed duration, then reports throughput and average per-message
//          latency.
//
// Two-Copy Data Path (recv side):
// ===============================
//
//   Hardware              Kernel Space                    User Space
//  +--------+   Copy 1   +----------------+   Copy 2   +------------+
//  |  NIC   | ---------> | Socket recv   | ---------> | recv buffer|
//  | RX ring|  DMA/driver| buffer (sk_buf)|  recv()    | (heap)     |
//  +--------+            +----------------+            +------------+
//
//   Copy 1: NIC RX ring  -->  Kernel socket buffer (sk_buff)
//           DMA engine transfers incoming packet data into kernel memory.
//
//   Copy 2: Kernel buffer -->  User-space heap buffer
//           Performed by the CPU during the recv() system call.
//
// Usage:
//   ./MT25082_Part_A1_Client <server_ip> <port> <msg_size> <threads> <duration>
//
// Example:
//   ./MT25082_Part_A1_Client 10.0.0.1 9090 4096 4 10
// =============================================================================

#include "MT25082_common.h"

// ===========================================================================
//  Per-thread result structure
// ===========================================================================
//  Each thread writes its results here.  No sharing between threads — the
//  main thread reads these only after pthread_join(), so no locks needed.
// ---------------------------------------------------------------------------
typedef struct {
    size_t total_bytes;         /* Total bytes received by this thread       */
    size_t total_messages;      /* Number of complete messages received      */
    double elapsed_us;          /* Wall-clock time for this thread (µs)      */
} thread_result_t;

// ===========================================================================
//  Extended thread arguments (client-specific)
// ===========================================================================
//  Wraps the common thread_args_t with the server address information and
//  a pointer to the result structure.
// ---------------------------------------------------------------------------
typedef struct {
    char           server_ip[64]; /* Server IP address string               */
    int            port;          /* Server port number                     */
    size_t         msg_size;      /* Expected total message size (bytes)    */
    int            duration_sec;  /* How long to receive (seconds)          */
    thread_result_t *result;      /* Where to write results (caller-owned)  */
} client_thread_args_t;

// ===========================================================================
//  client_thread
// ===========================================================================
//  Thread entry point.  Each thread:
//    1. Opens its own TCP connection to the server.
//    2. Allocates a private heap buffer for receiving.
//    3. Loops calling recv() until the duration expires.
//    4. Records bytes received, message count, and elapsed time.
// ---------------------------------------------------------------------------
static void *client_thread(void *arg)
{
    /* ---- Unpack arguments --------------------------------------------- */
    client_thread_args_t *cargs = (client_thread_args_t *)arg;
    const char *server_ip   = cargs->server_ip;
    int         port        = cargs->port;
    size_t      msg_size    = cargs->msg_size;
    int         duration_s  = cargs->duration_sec;
    thread_result_t *result = cargs->result;

    /* Initialise result */
    result->total_bytes    = 0;
    result->total_messages = 0;
    result->elapsed_us     = 0.0;

    /* ---- Create TCP socket -------------------------------------------- */
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("[Client] socket");
        return NULL;
    }

    /* ---- Connect to server -------------------------------------------- */
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port   = htons((uint16_t)port);

    if (inet_pton(AF_INET, server_ip, &serv_addr.sin_addr) <= 0) {
        fprintf(stderr, "[Client] Invalid server IP: %s\n", server_ip);
        close(sock_fd);
        return NULL;
    }

    if (connect(sock_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("[Client] connect");
        close(sock_fd);
        return NULL;
    }

    /* Disable Nagle for latency-sensitive measurements */
    int flag = 1;
    setsockopt(sock_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    printf("[Client] Thread %lu connected to %s:%d\n",
           (unsigned long)pthread_self(), server_ip, port);

    /* ---- Allocate private receive buffer on the heap ------------------ */
    /*
     * Each thread gets its own buffer — no sharing, no locks needed.
     * The buffer size matches msg_size so we can count complete messages.
     */
    char *recv_buf = (char *)malloc(msg_size);
    if (recv_buf == NULL) {
        perror("[Client] malloc recv_buf");
        close(sock_fd);
        return NULL;
    }

    /* ---- Receive loop ------------------------------------------------- */
    double start_time    = get_time_us();
    double deadline_us   = start_time + (double)duration_s * 1e6;
    size_t bytes_in_msg  = 0;   /* Tracks partial progress toward one msg */

    while (get_time_us() < deadline_us) {
        /*
         * recv() performs Copy 2 of the two-copy path:
         *   Kernel socket buffer (sk_buff)  -->  User-space heap buffer.
         * The CPU copies data from kernel memory into recv_buf.
         */
        ssize_t n = recv(sock_fd,
                         recv_buf + bytes_in_msg,
                         msg_size - bytes_in_msg,
                         0);

        if (n <= 0) {
            if (n == 0) {
                /* Server closed the connection */
                printf("[Client] Thread %lu: server disconnected\n",
                       (unsigned long)pthread_self());
            } else if (errno == EINTR) {
                continue;   /* Signal interrupted recv(), retry */
            } else {
                perror("[Client] recv");
            }
            break;
        }

        result->total_bytes += (size_t)n;
        bytes_in_msg        += (size_t)n;

        /* Check if we've received a complete message */
        if (bytes_in_msg >= msg_size) {
            result->total_messages++;
            bytes_in_msg = 0;   /* Reset for the next message */
        }
    }

    double end_time = get_time_us();
    result->elapsed_us = end_time - start_time;

    /* ---- Cleanup ------------------------------------------------------ */
    free(recv_buf);
    close(sock_fd);

    return NULL;
}

// ===========================================================================
//  main
// ===========================================================================
int main(int argc, char *argv[])
{
    /* ---- Parse command-line arguments --------------------------------- */
    if (argc != 6) {
        fprintf(stderr,
                "Usage: %s <server_ip> <port> <msg_size> <threads> <duration_sec>\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    const char *server_ip = argv[1];
    int         port      = atoi(argv[2]);
    size_t      msg_size  = (size_t)atol(argv[3]);
    int         n_threads = atoi(argv[4]);
    int         duration  = atoi(argv[5]);

    /* Validate inputs */
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "[Client] Invalid port: %d\n", port);
        return EXIT_FAILURE;
    }
    if (msg_size == 0) {
        fprintf(stderr, "[Client] Message size must be > 0\n");
        return EXIT_FAILURE;
    }
    if (n_threads <= 0) {
        fprintf(stderr, "[Client] Thread count must be > 0\n");
        return EXIT_FAILURE;
    }
    if (duration <= 0) {
        fprintf(stderr, "[Client] Duration must be > 0\n");
        return EXIT_FAILURE;
    }

    printf("[Client] Two-Copy Baseline (send/recv)\n");
    printf("[Client] Server: %s:%d | msg_size: %zu | threads: %d | "
           "duration: %d s\n",
           server_ip, port, msg_size, n_threads, duration);

    /* Ignore SIGPIPE */
    signal(SIGPIPE, SIG_IGN);

    /* ---- Allocate per-thread structures (all on the heap) ------------- */
    pthread_t            *tids    = malloc(sizeof(pthread_t)            * n_threads);
    client_thread_args_t *targs   = malloc(sizeof(client_thread_args_t) * n_threads);
    thread_result_t      *results = malloc(sizeof(thread_result_t)      * n_threads);

    if (tids == NULL || targs == NULL || results == NULL) {
        perror("[Client] malloc");
        free(tids);
        free(targs);
        free(results);
        return EXIT_FAILURE;
    }

    /* ---- Launch threads ----------------------------------------------- */
    for (int i = 0; i < n_threads; i++) {
        strncpy(targs[i].server_ip, server_ip, sizeof(targs[i].server_ip) - 1);
        targs[i].server_ip[sizeof(targs[i].server_ip) - 1] = '\0';
        targs[i].port         = port;
        targs[i].msg_size     = msg_size;
        targs[i].duration_sec = duration;
        targs[i].result       = &results[i];

        if (pthread_create(&tids[i], NULL, client_thread, &targs[i]) != 0) {
            perror("[Client] pthread_create");
            /* Mark this thread as invalid */
            tids[i] = 0;
        }
    }

    /* ---- Join threads and aggregate results --------------------------- */
    size_t aggregate_bytes    = 0;
    size_t aggregate_messages = 0;
    double max_elapsed_us     = 0.0;

    for (int i = 0; i < n_threads; i++) {
        if (tids[i] != 0) {
            pthread_join(tids[i], NULL);
        }

        aggregate_bytes    += results[i].total_bytes;
        aggregate_messages += results[i].total_messages;

        if (results[i].elapsed_us > max_elapsed_us) {
            max_elapsed_us = results[i].elapsed_us;
        }

        /* Per-thread summary */
        double thr_s    = results[i].elapsed_us / 1e6;
        double thr_gbps = (thr_s > 0.0)
            ? ((double)results[i].total_bytes * 8.0) / (thr_s * 1e9)
            : 0.0;
        double avg_lat  = (results[i].total_messages > 0)
            ? results[i].elapsed_us / (double)results[i].total_messages
            : 0.0;

        printf("[Client] Thread %d: %zu bytes, %zu msgs, %.2f s, "
               "%.4f Gbps, avg latency %.2f µs/msg\n",
               i, results[i].total_bytes, results[i].total_messages,
               thr_s, thr_gbps, avg_lat);
    }

    /* ---- Aggregate summary -------------------------------------------- */
    double total_s    = max_elapsed_us / 1e6;
    double agg_gbps   = (total_s > 0.0)
        ? ((double)aggregate_bytes * 8.0) / (total_s * 1e9)
        : 0.0;
    double avg_lat_us = (aggregate_messages > 0)
        ? max_elapsed_us / (double)aggregate_messages
        : 0.0;

    printf("\n========== AGGREGATE RESULTS ==========\n");
    printf("Total bytes received : %zu\n", aggregate_bytes);
    printf("Total messages       : %zu\n", aggregate_messages);
    printf("Wall-clock time      : %.2f s\n", total_s);
    printf("Aggregate throughput : %.4f Gbps\n", agg_gbps);
    printf("Avg latency/msg      : %.2f µs\n", avg_lat_us);
    printf("========================================\n");

    /* ---- Cleanup ------------------------------------------------------ */
    free(tids);
    free(targs);
    free(results);

    return EXIT_SUCCESS;
}
