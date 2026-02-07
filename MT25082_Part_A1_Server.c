// Roll No: MT25082
// =============================================================================
// File:    MT25082_Part_A1_Server.c
// Purpose: Two-Copy Baseline TCP Server
//
//          This server implements the standard two-copy data path using
//          send() / recv().  It accepts multiple concurrent clients, spawns
//          one pthread per client, and continuously sends a heap-allocated
//          message_t (8 string fields) until the client disconnects.
//
// Two-Copy Data Path (send):
// ==========================
//
//   User Space                    Kernel Space                   Hardware
//  +-----------+    Copy 1       +----------------+   Copy 2   +--------+
//  | message_t | ------------->  | Socket send    | ---------> |  NIC   |
//  | (heap)    |  send() syscall | buffer (sk_buf)|  DMA/driver| TX ring|
//  +-----------+                 +----------------+            +--------+
//
//   Copy 1: User-space heap buffer  -->  Kernel socket buffer (sk_buff)
//           Performed by the kernel during the send() system call.
//           The CPU copies data from the user-space virtual address into
//           a kernel-allocated sk_buff structure.
//
//   Copy 2: Kernel socket buffer    -->  NIC TX ring buffer
//           Performed by the NIC driver / DMA engine.  The kernel hands
//           the sk_buff to the network driver, which programs the NIC's
//           DMA controller to read the data from kernel memory into the
//           hardware transmit ring.
//
// Usage:
//   ./MT25082_Part_A1_Server <port> <message_size_bytes>
//
// Example:
//   ./MT25082_Part_A1_Server 9090 4096
// =============================================================================

#include "MT25082_common.h"

// ---------------------------------------------------------------------------
//  Global flag for clean SIGINT shutdown.
//  Declared volatile so every thread sees updates immediately.
//  This is the ONLY global; no shared buffers exist.
// ---------------------------------------------------------------------------
static volatile sig_atomic_t g_running = 1;

// ---------------------------------------------------------------------------
//  SIGINT handler — sets the flag so accept() / send() loops exit cleanly.
// ---------------------------------------------------------------------------
static void sigint_handler(int signo)
{
    (void)signo;        /* Unused parameter */
    g_running = 0;
}

// ===========================================================================
//  client_handler
// ===========================================================================
//  Thread entry point.  Each connected client gets its own thread running
//  this function.  The thread:
//    1. Allocates its own message_t on the heap  (no shared buffers).
//    2. Fills the message with deterministic data.
//    3. Sends the 8 fields one by one using send() in a loop.
//    4. Stops when the client disconnects or the server is shutting down.
//    5. Frees all heap memory and closes the socket.
// ---------------------------------------------------------------------------
static void *client_handler(void *arg)
{
    /* ---- Unpack per-thread arguments ---------------------------------- */
    thread_args_t *targs = (thread_args_t *)arg;
    int    client_fd = targs->sock_fd;
    size_t msg_size  = targs->msg_size;
    free(targs);   /* Heap-allocated by the accept loop; no longer needed */

    printf("[Server] Thread %lu: handling client fd=%d, msg_size=%zu\n",
           (unsigned long)pthread_self(), client_fd, msg_size);

    /* ---- Allocate message on the heap (per-thread, no sharing) -------- */
    /*
     * Heap allocation ensures:
     *   • Sizes determined at runtime are supported.
     *   • Each thread has private buffers — thread-safe without locks.
     *   • Faithfully represents the user-space buffer that will be
     *     copied into kernel space (Copy 1) during send().
     */
    message_t msg;
    allocate_message(&msg, msg_size);
    fill_message(&msg, msg_size);

    /* Pre-compute per-field sizes (mirrors allocate_message logic) */
    size_t per_field = msg_size / NUM_FIELDS;
    size_t remainder = msg_size % NUM_FIELDS;

    /* ---- Counters for optional throughput reporting -------------------- */
    size_t total_bytes_sent = 0;
    size_t total_messages   = 0;
    double start_time       = get_time_us();

    /* ---- Main send loop ----------------------------------------------- */
    while (g_running) {
        int send_failed = 0;

        for (int i = 0; i < NUM_FIELDS; i++) {
            size_t field_size = per_field +
                                ((i == NUM_FIELDS - 1) ? remainder : 0);

            size_t bytes_sent = 0;

            /* Send the complete field, handling partial sends */
            while (bytes_sent < field_size) {
                /*
                 * =========================================================
                 *  COPY 1 OCCURS HERE — inside send()
                 * =========================================================
                 *  The kernel copies data from the user-space heap buffer
                 *  (msg.field[i] + bytes_sent) into a kernel-managed
                 *  sk_buff in the socket's send buffer.
                 *
                 *  After this call returns, the application buffer is free
                 *  to be modified — the kernel holds its own copy.
                 *
                 *  COPY 2 happens asynchronously when the NIC driver's
                 *  DMA engine transfers the sk_buff contents from kernel
                 *  memory into the NIC's hardware TX ring buffer.
                 * =========================================================
                 */
                ssize_t ret = send(client_fd,
                                   msg.field[i] + bytes_sent,
                                   field_size   - bytes_sent,
                                   MSG_NOSIGNAL);

                if (ret <= 0) {
                    if (ret == 0) {
                        /* Client closed the connection gracefully */
                        printf("[Server] Thread %lu: client disconnected\n",
                               (unsigned long)pthread_self());
                    } else {
                        /* send() error — check if it's a fatal condition */
                        if (errno == EPIPE || errno == ECONNRESET) {
                            printf("[Server] Thread %lu: client gone (%s)\n",
                                   (unsigned long)pthread_self(),
                                   strerror(errno));
                        } else if (errno == EINTR) {
                            continue;   /* Interrupted by signal, retry */
                        } else {
                            perror("[Server] send");
                        }
                    }
                    send_failed = 1;
                    break;
                }

                bytes_sent += (size_t)ret;
            }

            if (send_failed) {
                break;
            }
        }

        if (send_failed) {
            break;  /* Exit the outer while-loop */
        }

        total_bytes_sent += msg_size;
        total_messages++;
    }

    /* ---- Report per-thread statistics --------------------------------- */
    double elapsed_us = get_time_us() - start_time;
    double elapsed_s  = elapsed_us / 1e6;
    double throughput_gbps = (elapsed_s > 0.0)
        ? ((double)total_bytes_sent * 8.0) / (elapsed_s * 1e9)
        : 0.0;

    printf("[Server] Thread %lu: sent %zu messages (%zu bytes) in %.2f s "
           "— %.4f Gbps\n",
           (unsigned long)pthread_self(),
           total_messages, total_bytes_sent, elapsed_s, throughput_gbps);

    /* ---- Cleanup: free heap buffers, close socket --------------------- */
    free_message(&msg);
    close(client_fd);

    return NULL;
}

// ===========================================================================
//  main
// ===========================================================================
int main(int argc, char *argv[])
{
    /* ---- Parse command-line arguments --------------------------------- */
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <port> <message_size_bytes>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int    port     = atoi(argv[1]);
    size_t msg_size = (size_t)atol(argv[2]);

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "[Server] Invalid port: %d\n", port);
        return EXIT_FAILURE;
    }
    if (msg_size == 0) {
        fprintf(stderr, "[Server] Message size must be > 0\n");
        return EXIT_FAILURE;
    }

    printf("[Server] Two-Copy Baseline (send/recv)\n");
    printf("[Server] Port: %d | Message size: %zu bytes\n", port, msg_size);

    /* ---- Install SIGINT handler for graceful shutdown ------------------ */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) < 0) {
        perror("[Server] sigaction");
        return EXIT_FAILURE;
    }

    /* Ignore SIGPIPE so broken-pipe errors are returned via errno */
    signal(SIGPIPE, SIG_IGN);

    /* ---- Create listening TCP socket ---------------------------------- */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("[Server] socket");
        return EXIT_FAILURE;
    }

    /* Allow immediate port reuse after restart */
    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("[Server] setsockopt SO_REUSEADDR");
        close(listen_fd);
        return EXIT_FAILURE;
    }

    /* ---- Bind to the specified port ----------------------------------- */
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family      = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port        = htons((uint16_t)port);

    if (bind(listen_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("[Server] bind");
        close(listen_fd);
        return EXIT_FAILURE;
    }

    /* ---- Start listening ---------------------------------------------- */
    if (listen(listen_fd, BACKLOG) < 0) {
        perror("[Server] listen");
        close(listen_fd);
        return EXIT_FAILURE;
    }

    printf("[Server] Listening on port %d … (Ctrl+C to stop)\n", port);

    /* ---- Accept loop: one pthread per client -------------------------- */
    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(listen_fd,
                               (struct sockaddr *)&client_addr,
                               &addr_len);

        if (client_fd < 0) {
            if (errno == EINTR) {
                /* accept() interrupted by SIGINT — check g_running */
                continue;
            }
            perror("[Server] accept");
            continue;
        }

        printf("[Server] Accepted connection from %s:%d (fd=%d)\n",
               inet_ntoa(client_addr.sin_addr),
               ntohs(client_addr.sin_port),
               client_fd);

        /* Disable Nagle's algorithm for lower latency measurements */
        int flag = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        /*
         * Allocate thread_args_t on the heap so it survives past this
         * loop iteration.  The thread is responsible for freeing it.
         */
        thread_args_t *targs = (thread_args_t *)malloc(sizeof(thread_args_t));
        if (targs == NULL) {
            perror("[Server] malloc thread_args");
            close(client_fd);
            continue;
        }
        targs->sock_fd      = client_fd;
        targs->msg_size     = msg_size;
        targs->duration_sec = 0;   /* Not used by server; client decides */

        /* Spawn a detached thread — no join needed */
        pthread_t tid;
        if (pthread_create(&tid, NULL, client_handler, targs) != 0) {
            perror("[Server] pthread_create");
            free(targs);
            close(client_fd);
            continue;
        }
        pthread_detach(tid);
    }

    /* ---- Shutdown ----------------------------------------------------- */
    printf("\n[Server] Shutting down …\n");
    close(listen_fd);

    return EXIT_SUCCESS;
}
