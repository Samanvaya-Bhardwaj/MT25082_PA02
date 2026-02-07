// Roll No: MT25082
// =============================================================================
// File:    MT25082_Part_A3_Client.c
// Purpose: Zero-Copy TCP Client (paired with A3 MSG_ZEROCOPY server)
//
//          This client is structurally identical to the A1 and A2 clients.
//          It uses recv() to consume data from the server.
//
//          The client has NO awareness of zero-copy:
//            • Zero-copy (MSG_ZEROCOPY) is a sender-side optimisation.
//              It only affects how the server hands data to the kernel.
//            • On the receive side, the kernel still copies data from
//              sk_buffs into user-space via recv() — this is unchanged
//              across all three implementations (A1, A2, A3).
//            • The TCP byte-stream abstraction makes the three server
//              implementations indistinguishable from the client's
//              perspective.
//
// Usage:
//   ./MT25082_Part_A3_Client <server_ip> <port> <msg_size> <threads> <duration>
//
// Example:
//   ./MT25082_Part_A3_Client 10.0.0.1 9092 65536 4 10
// =============================================================================

#include "MT25082_common.h"

// ===========================================================================
//  Per-thread result structure
// ===========================================================================
typedef struct {
    size_t total_bytes;         /* Total bytes received by this thread       */
    size_t total_messages;      /* Number of complete messages received      */
    double elapsed_us;          /* Wall-clock time for this thread (µs)      */
} thread_result_t;

// ===========================================================================
//  Extended thread arguments (client-specific)
// ===========================================================================
typedef struct {
    char            server_ip[64]; /* Server IP address string              */
    int             port;          /* Server port number                    */
    size_t          msg_size;      /* Expected total message size (bytes)   */
    int             duration_sec;  /* How long to receive (seconds)         */
    thread_result_t *result;       /* Where to write results (caller-owned) */
} client_thread_args_t;

// ===========================================================================
//  client_thread
// ===========================================================================
//  Each thread opens its own TCP connection and receives data for the
//  specified duration.  The recv() call is the same regardless of whether
//  the server uses send(), sendmsg(), or sendmsg()+MSG_ZEROCOPY — the
//  kernel→user copy on the receive path is invariant.
// ---------------------------------------------------------------------------
static void *client_thread(void *arg)
{
    /* ---- Unpack arguments --------------------------------------------- */
    client_thread_args_t *cargs = (client_thread_args_t *)arg;
    const char      *server_ip  = cargs->server_ip;
    int              port       = cargs->port;
    size_t           msg_size   = cargs->msg_size;
    int              duration_s = cargs->duration_sec;
    thread_result_t *result     = cargs->result;

    result->total_bytes    = 0;
    result->total_messages = 0;
    result->elapsed_us     = 0.0;

    /* ---- Create TCP socket -------------------------------------------- */
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("[Client-A3] socket");
        return NULL;
    }

    /* ---- Connect to server -------------------------------------------- */
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port   = htons((uint16_t)port);

    if (inet_pton(AF_INET, server_ip, &serv_addr.sin_addr) <= 0) {
        fprintf(stderr, "[Client-A3] Invalid server IP: %s\n", server_ip);
        close(sock_fd);
        return NULL;
    }

    if (connect(sock_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("[Client-A3] connect");
        close(sock_fd);
        return NULL;
    }

    /* Disable Nagle for consistent latency measurements */
    int flag = 1;
    setsockopt(sock_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    printf("[Client-A3] Thread %lu connected to %s:%d\n",
           (unsigned long)pthread_self(), server_ip, port);

    /* ---- Allocate private receive buffer on the heap ------------------ */
    char *recv_buf = (char *)malloc(msg_size);
    if (recv_buf == NULL) {
        perror("[Client-A3] malloc recv_buf");
        close(sock_fd);
        return NULL;
    }

    /* ---- Receive loop ------------------------------------------------- */
    double start_time   = get_time_us();
    double deadline_us  = start_time + (double)duration_s * 1e6;
    size_t bytes_in_msg = 0;

    while (get_time_us() < deadline_us) {
        ssize_t n = recv(sock_fd,
                         recv_buf + bytes_in_msg,
                         msg_size - bytes_in_msg,
                         0);

        if (n <= 0) {
            if (n == 0) {
                printf("[Client-A3] Thread %lu: server disconnected\n",
                       (unsigned long)pthread_self());
            } else if (errno == EINTR) {
                continue;
            } else {
                perror("[Client-A3] recv");
            }
            break;
        }

        result->total_bytes += (size_t)n;
        bytes_in_msg        += (size_t)n;

        if (bytes_in_msg >= msg_size) {
            result->total_messages++;
            bytes_in_msg = 0;
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

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "[Client-A3] Invalid port: %d\n", port);
        return EXIT_FAILURE;
    }
    if (msg_size == 0) {
        fprintf(stderr, "[Client-A3] Message size must be > 0\n");
        return EXIT_FAILURE;
    }
    if (n_threads <= 0) {
        fprintf(stderr, "[Client-A3] Thread count must be > 0\n");
        return EXIT_FAILURE;
    }
    if (duration <= 0) {
        fprintf(stderr, "[Client-A3] Duration must be > 0\n");
        return EXIT_FAILURE;
    }

    printf("[Client-A3] Zero-Copy Client (paired with MSG_ZEROCOPY server)\n");
    printf("[Client-A3] Server: %s:%d | msg_size: %zu | threads: %d | "
           "duration: %d s\n",
           server_ip, port, msg_size, n_threads, duration);

    signal(SIGPIPE, SIG_IGN);

    /* ---- Allocate per-thread structures ------------------------------- */
    pthread_t            *tids    = malloc(sizeof(pthread_t)            * n_threads);
    client_thread_args_t *targs   = malloc(sizeof(client_thread_args_t) * n_threads);
    thread_result_t      *results = malloc(sizeof(thread_result_t)      * n_threads);

    if (tids == NULL || targs == NULL || results == NULL) {
        perror("[Client-A3] malloc");
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
            perror("[Client-A3] pthread_create");
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

        double thr_s    = results[i].elapsed_us / 1e6;
        double thr_gbps = (thr_s > 0.0)
            ? ((double)results[i].total_bytes * 8.0) / (thr_s * 1e9)
            : 0.0;
        double avg_lat  = (results[i].total_messages > 0)
            ? results[i].elapsed_us / (double)results[i].total_messages
            : 0.0;

        printf("[Client-A3] Thread %d: %zu bytes, %zu msgs, %.2f s, "
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

    printf("\n========== AGGREGATE RESULTS (A3 — Zero-Copy) ==========\n");
    printf("Total bytes received : %zu\n", aggregate_bytes);
    printf("Total messages       : %zu\n", aggregate_messages);
    printf("Wall-clock time      : %.2f s\n", total_s);
    printf("Aggregate throughput : %.4f Gbps\n", agg_gbps);
    printf("Avg latency/msg      : %.2f µs\n", avg_lat_us);
    printf("=========================================================\n");

    /* ---- Cleanup ------------------------------------------------------ */
    free(tids);
    free(targs);
    free(results);

    return EXIT_SUCCESS;
}
