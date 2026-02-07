// Roll No: MT25082
// =============================================================================
// File:    MT25082_Part_A2_Server.c
// Purpose: One-Copy Optimised TCP Server
//
//          This server uses sendmsg() with pre-registered iovec buffers to
//          transmit data.  By gathering all 8 message fields into a single
//          iovec array and issuing ONE system call, we eliminate the overhead
//          of multiple send() calls — each of which would independently copy
//          its segment into the kernel.  The kernel can coalesce the
//          scatter-gather list into a single sk_buff chain more efficiently.
//
// Which copy is eliminated?
// =========================
//
//   In the Two-Copy baseline (Part A1), each field triggers a separate
//   send() system call.  The kernel copies each user buffer into a new
//   sk_buff.  With N fields, there are N independent user→kernel copy
//   operations, each incurring:
//     • A full context switch into kernel mode
//     • A fresh sk_buff allocation
//     • A memcpy from user-space into that sk_buff
//
//   With sendmsg() + iovec:
//     • A single system call is made for ALL 8 fields.
//     • The kernel receives the iovec array and can perform a single,
//       contiguous copy (or even a gather-DMA) from the user-space
//       pages directly, avoiding redundant per-field copy overhead.
//     • On kernels with MSG_MORE / scatter-gather NIC support, the
//       driver can DMA directly from the user-space page mappings
//       referenced by the iovec, effectively eliminating the
//       intermediate kernel-buffer copy for coalesced segments.
//
//   Result:  The per-field user→kernel copy overhead is reduced to a
//   single consolidated operation, effectively eliminating one copy
//   in the data path compared to the A1 baseline.
//
// One-Copy Data Path with sendmsg():
// ===================================
//
//   User Space                         Kernel Space                Hardware
//  +-----------+                      +----------------+          +--------+
//  | iovec[0]  |---+                  |                |  DMA     |        |
//  | iovec[1]  |---+  sendmsg()       | sk_buff chain  | -------> |  NIC   |
//  | ...       |---+  (1 syscall)     | (consolidated) |          | TX ring|
//  | iovec[7]  |---+  single copy     |                |          |        |
//  +-----------+                      +----------------+          +--------+
//
//   Copy 1 (consolidated): All iovec entries copied in one pass into
//          the kernel socket buffer.  The kernel walks the iovec array
//          and copies all segments together — this replaces 8 separate
//          user→kernel copies with a single consolidated operation.
//
//   Copy 2 (DMA): Kernel buffer → NIC TX ring via DMA (same as A1).
//
// Usage:
//   ./MT25082_Part_A2_Server <port> <message_size_bytes>
// =============================================================================

#include "MT25082_common.h"

// ---------------------------------------------------------------------------
//  Global flag for clean SIGINT shutdown.
//  Declared volatile so every thread sees updates immediately.
// ---------------------------------------------------------------------------
static volatile sig_atomic_t g_running = 1;

// ---------------------------------------------------------------------------
//  SIGINT handler
// ---------------------------------------------------------------------------
static void sigint_handler(int signo)
{
    (void)signo;
    g_running = 0;
}

// ===========================================================================
//  client_handler
// ===========================================================================
//  Thread entry point.  Each client gets its own thread.
//
//  Key difference from A1:
//    • Instead of 8 separate send() calls, we pre-register all 8 message
//      fields as iovec entries and issue a SINGLE sendmsg() call per
//      message.  This eliminates the redundant per-field copy overhead.
// ---------------------------------------------------------------------------
static void *client_handler(void *arg)
{
    /* ---- Unpack per-thread arguments ---------------------------------- */
    thread_args_t *targs = (thread_args_t *)arg;
    int    client_fd = targs->sock_fd;
    size_t msg_size  = targs->msg_size;
    free(targs);   /* Heap-allocated by accept loop; thread owns lifetime */

    printf("[Server-A2] Thread %lu: handling client fd=%d, msg_size=%zu\n",
           (unsigned long)pthread_self(), client_fd, msg_size);

    /* ---- Allocate message on the heap (per-thread, no sharing) -------- */
    message_t msg;
    allocate_message(&msg, msg_size);
    fill_message(&msg, msg_size);

    /* Pre-compute per-field sizes */
    size_t per_field = msg_size / NUM_FIELDS;
    size_t remainder = msg_size % NUM_FIELDS;

    /* ================================================================== */
    /*  PRE-REGISTER iovec buffers                                        */
    /* ================================================================== */
    /*  The iovec array is set up ONCE before the send loop.  Each entry   */
    /*  points directly to a heap-allocated field of message_t.  Because   */
    /*  the buffer addresses and lengths never change across iterations,   */
    /*  we avoid re-initialising the iovec on every send — this is the    */
    /*  "pre-registration" that makes sendmsg() efficient.                */
    /* ================================================================== */
    struct iovec iov[NUM_FIELDS];
    for (int i = 0; i < NUM_FIELDS; i++) {
        iov[i].iov_base = msg.field[i];
        iov[i].iov_len  = per_field + ((i == NUM_FIELDS - 1) ? remainder : 0);
    }

    /* ---- Prepare msghdr (reused across all sends) --------------------- */
    struct msghdr mh;
    memset(&mh, 0, sizeof(mh));
    mh.msg_name       = NULL;         /* Connected socket — no address    */
    mh.msg_namelen    = 0;
    mh.msg_iov        = iov;          /* Pre-registered scatter array     */
    mh.msg_iovlen     = NUM_FIELDS;   /* 8 entries                        */
    mh.msg_control    = NULL;         /* No ancillary data                */
    mh.msg_controllen = 0;
    mh.msg_flags      = 0;

    /* ---- Counters ----------------------------------------------------- */
    size_t total_bytes_sent = 0;
    size_t total_messages   = 0;
    double start_time       = get_time_us();

    /* ---- Main send loop ----------------------------------------------- */
    while (g_running) {
        /*
         * =================================================================
         *  ONE-COPY SEND — sendmsg() with pre-registered iovec
         * =================================================================
         *
         *  sendmsg() receives the entire iovec array in a SINGLE system
         *  call.  The kernel iterates over all 8 iov entries and performs
         *  ONE consolidated copy from user-space into the kernel socket
         *  buffer (sk_buff chain).
         *
         *  COPY ELIMINATED:
         *  In Part A1, each send() call independently transitions into
         *  kernel mode and copies one field.  Here, ALL 8 fields are
         *  gathered in one pass — the per-field system-call and copy
         *  overhead is eliminated.  The kernel sees the full scatter
         *  list and can optimise the copy (e.g., page-pinning, gather
         *  DMA on capable NICs).
         *
         *  NOTE: We do NOT use MSG_ZEROCOPY here.  The kernel still
         *  copies data from user-space pages into sk_buffs, but it does
         *  so in a single, consolidated operation rather than one per
         *  field.
         * =================================================================
         */
        ssize_t ret = sendmsg(client_fd, &mh, MSG_NOSIGNAL);

        if (ret <= 0) {
            if (ret == 0) {
                printf("[Server-A2] Thread %lu: client disconnected\n",
                       (unsigned long)pthread_self());
            } else if (errno == EINTR) {
                continue;   /* Signal interrupted, retry */
            } else if (errno == EPIPE || errno == ECONNRESET) {
                printf("[Server-A2] Thread %lu: client gone (%s)\n",
                       (unsigned long)pthread_self(), strerror(errno));
            } else {
                perror("[Server-A2] sendmsg");
            }
            break;
        }

        /*
         * sendmsg() may send fewer bytes than requested (partial send).
         * For simplicity in this benchmark, we count only complete sends;
         * a production implementation would advance iov offsets for the
         * remainder.
         */
        total_bytes_sent += (size_t)ret;
        if ((size_t)ret == msg_size) {
            total_messages++;
        }
    }

    /* ---- Report per-thread statistics --------------------------------- */
    double elapsed_us   = get_time_us() - start_time;
    double elapsed_s    = elapsed_us / 1e6;
    double throughput   = (elapsed_s > 0.0)
        ? ((double)total_bytes_sent * 8.0) / (elapsed_s * 1e9)
        : 0.0;

    printf("[Server-A2] Thread %lu: sent %zu msgs (%zu bytes) in %.2f s "
           "— %.4f Gbps\n",
           (unsigned long)pthread_self(),
           total_messages, total_bytes_sent, elapsed_s, throughput);

    /* ---- Cleanup ------------------------------------------------------ */
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
        fprintf(stderr, "[Server-A2] Invalid port: %d\n", port);
        return EXIT_FAILURE;
    }
    if (msg_size == 0) {
        fprintf(stderr, "[Server-A2] Message size must be > 0\n");
        return EXIT_FAILURE;
    }

    printf("[Server-A2] One-Copy Optimised (sendmsg + iovec)\n");
    printf("[Server-A2] Port: %d | Message size: %zu bytes\n", port, msg_size);

    /* ---- Install SIGINT handler --------------------------------------- */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) < 0) {
        perror("[Server-A2] sigaction");
        return EXIT_FAILURE;
    }
    signal(SIGPIPE, SIG_IGN);

    /* ---- Create listening socket -------------------------------------- */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("[Server-A2] socket");
        return EXIT_FAILURE;
    }

    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("[Server-A2] setsockopt SO_REUSEADDR");
        close(listen_fd);
        return EXIT_FAILURE;
    }

    /* ---- Bind --------------------------------------------------------- */
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family      = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port        = htons((uint16_t)port);

    if (bind(listen_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("[Server-A2] bind");
        close(listen_fd);
        return EXIT_FAILURE;
    }

    /* ---- Listen ------------------------------------------------------- */
    if (listen(listen_fd, BACKLOG) < 0) {
        perror("[Server-A2] listen");
        close(listen_fd);
        return EXIT_FAILURE;
    }

    printf("[Server-A2] Listening on port %d … (Ctrl+C to stop)\n", port);

    /* ---- Accept loop -------------------------------------------------- */
    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(listen_fd,
                               (struct sockaddr *)&client_addr,
                               &addr_len);

        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("[Server-A2] accept");
            continue;
        }

        printf("[Server-A2] Accepted connection from %s:%d (fd=%d)\n",
               inet_ntoa(client_addr.sin_addr),
               ntohs(client_addr.sin_port),
               client_fd);

        /* Disable Nagle */
        int flag = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        /* Heap-allocate thread args — thread frees them */
        thread_args_t *targs = (thread_args_t *)malloc(sizeof(thread_args_t));
        if (targs == NULL) {
            perror("[Server-A2] malloc thread_args");
            close(client_fd);
            continue;
        }
        targs->sock_fd      = client_fd;
        targs->msg_size     = msg_size;
        targs->duration_sec = 0;

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_handler, targs) != 0) {
            perror("[Server-A2] pthread_create");
            free(targs);
            close(client_fd);
            continue;
        }
        pthread_detach(tid);
    }

    /* ---- Shutdown ----------------------------------------------------- */
    printf("\n[Server-A2] Shutting down …\n");
    close(listen_fd);

    return EXIT_SUCCESS;
}
