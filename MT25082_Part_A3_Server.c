// Roll No: MT25082
// =============================================================================
// File:    MT25082_Part_A3_Server.c
// Purpose: Zero-Copy TCP Server using sendmsg() + MSG_ZEROCOPY
//
//          This server eliminates the user→kernel data copy entirely.
//          Instead of copying user-space buffers into kernel sk_buffs, the
//          kernel pins the user-space pages and lets the NIC DMA directly
//          from them.  A completion notification is delivered via the
//          socket's error queue so the application knows when the buffer
//          is safe to reuse or free.
//
// Zero-Copy Data Path:
// ====================
//
//   User Space                      Kernel Space                   Hardware
//  +-----------+                   +------------------+           +--------+
//  | iovec[0]  |--+                |                  |           |        |
//  | iovec[1]  |--+  sendmsg()     |  Page table pin  |  DMA     |  NIC   |
//  | ...       |--+  MSG_ZEROCOPY  |  (no memcpy!)    | -------> | TX ring|
//  | iovec[7]  |--+                |                  |           |        |
//  +-----------+  |                +------------------+           +--------+
//       |         |                        |
//       |         |    (user pages remain  |
//       |         |     pinned in memory)  |
//       |         |                        |
//       |         v                        v
//       |   +------------------------------------------+
//       |   | Completion notification via SO_EE_ORIGIN |
//       |   | delivered on socket error queue when NIC |
//       +<--| DMA is finished & pages are unpinned.    |
//           +------------------------------------------+
//
//   Step-by-step kernel behavior:
//   ────────────────────────────────────────────────────────────────────────
//   1. Application calls sendmsg(fd, &mh, MSG_ZEROCOPY).
//   2. Kernel validates the iovec, but does NOT copy data.  Instead it:
//        a. Pins the user-space pages in physical memory (get_user_pages).
//        b. Creates sk_buff structures whose frags[] point to those
//           physical pages (skb_fill_page_desc).
//        c. Queues the sk_buff for transmission.
//   3. The NIC driver programs a scatter-gather DMA descriptor that
//      references the pinned physical pages directly.
//   4. The NIC DMA engine reads data from user-space pages → TX ring.
//      *** NO kernel-buffer copy occurs at any point. ***
//   5. After the NIC confirms transmission (TX completion interrupt),
//      the kernel unpins the user pages and posts a completion
//      notification on the socket's error queue (SO_EE_ORIGIN_ZEROCOPY).
//   6. The application calls recvmsg(fd, …, MSG_ERRQUEUE) to drain
//      the completion notification and knows the buffer is safe to
//      reuse or free.
//
//   Copies compared to baseline:
//   ────────────────────────────────────────────────────────────────────────
//    A1 (two-copy):  user→kernel copy  +  kernel→NIC DMA  =  2 copies
//    A2 (one-copy):  consolidated user→kernel  +  DMA     =  ~1.x copies
//    A3 (zero-copy): page-pin (no copy)  +  direct DMA    =  0 copies
//   ────────────────────────────────────────────────────────────────────────
//
// Usage:
//   ./MT25082_Part_A3_Server <port> <message_size_bytes>
//
// Prerequisites:
//   • Linux kernel ≥ 4.14 (MSG_ZEROCOPY support for TCP).
//   • SO_ZEROCOPY socket option must be enabled on the socket.
// =============================================================================

#include "MT25082_common.h"

// ---------------------------------------------------------------------------
//  Additional headers required for zero-copy error-queue processing
// ---------------------------------------------------------------------------
#include <linux/errqueue.h>     /* SO_EE_ORIGIN_ZEROCOPY, sock_extended_err */

// ---------------------------------------------------------------------------
//  Global flag for clean SIGINT shutdown
// ---------------------------------------------------------------------------
static volatile sig_atomic_t g_running = 1;

static void sigint_handler(int signo)
{
    (void)signo;
    g_running = 0;
}

// ===========================================================================
//  drain_completions
// ===========================================================================
//  Drains zero-copy completion notifications from the socket error queue.
//
//  After a MSG_ZEROCOPY sendmsg(), the kernel will post a notification
//  on the socket's error queue once the NIC has finished DMA-ing the data
//  and the user-space pages have been unpinned.  We must read these
//  notifications so the kernel can reclaim internal tracking structures
//  and to confirm that our send buffers are safe to modify/free.
//
//  Parameters:
//      sock_fd          – the connected socket
//      pending_count    – pointer to the outstanding zero-copy send counter;
//                         decremented for each notification received
//
//  Returns:
//      Number of completions drained (0 if none available).
// ---------------------------------------------------------------------------
static int drain_completions(int sock_fd, size_t *pending_count)
{
    int completions = 0;

    /*
     * Control-message buffer sized for one sock_extended_err plus padding.
     * CMSG_SPACE accounts for alignment requirements.
     */
    char cmsg_buf[CMSG_SPACE(sizeof(struct sock_extended_err))];

    struct msghdr mh;
    memset(&mh, 0, sizeof(mh));
    mh.msg_control    = cmsg_buf;
    mh.msg_controllen = sizeof(cmsg_buf);

    /*
     * Non-blocking recvmsg with MSG_ERRQUEUE:
     *   • MSG_ERRQUEUE reads from the error queue, not the data stream.
     *   • MSG_DONTWAIT ensures we don't block if no notifications exist.
     */
    while (1) {
        mh.msg_controllen = sizeof(cmsg_buf);

        ssize_t ret = recvmsg(sock_fd, &mh, MSG_ERRQUEUE | MSG_DONTWAIT);
        if (ret < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;  /* No more completions pending */
            }
            if (errno == EINTR) {
                continue;
            }
            /* Unexpected error — log and stop draining */
            perror("[Server-A3] recvmsg MSG_ERRQUEUE");
            break;
        }

        /* Walk the control-message chain looking for zerocopy completions */
        struct cmsghdr *cm;
        for (cm = CMSG_FIRSTHDR(&mh); cm != NULL; cm = CMSG_NXTHDR(&mh, cm)) {
            if (cm->cmsg_level != SOL_IP && cm->cmsg_level != SOL_IPV6) {
                continue;
            }
            if (cm->cmsg_type != IP_RECVERR
#ifdef IPV6_RECVERR
                && cm->cmsg_type != IPV6_RECVERR
#endif
               ) {
                continue;
            }

            struct sock_extended_err *serr =
                (struct sock_extended_err *)CMSG_DATA(cm);

            if (serr->ee_origin != SO_EE_ORIGIN_ZEROCOPY) {
                continue;
            }

            /*
             * serr->ee_info  = highest completed send counter
             * serr->ee_data  = lowest  completed send counter
             *
             * The range [ee_data .. ee_info] tells us how many
             * zero-copy sends have been fully transmitted.
             */
            uint32_t lo = serr->ee_data;
            uint32_t hi = serr->ee_info;
            uint32_t range = hi - lo + 1;

            if (*pending_count >= range) {
                *pending_count -= range;
            } else {
                *pending_count = 0;
            }

            completions += (int)range;
        }
    }

    return completions;
}

// ===========================================================================
//  client_handler
// ===========================================================================
//  Thread entry point.  Uses MSG_ZEROCOPY sendmsg() with pre-registered
//  iovec buffers.  Tracks outstanding zero-copy sends and drains the error
//  queue periodically to prevent unbounded kernel resource consumption.
// ---------------------------------------------------------------------------
static void *client_handler(void *arg)
{
    /* ---- Unpack per-thread arguments ---------------------------------- */
    thread_args_t *targs = (thread_args_t *)arg;
    int    client_fd = targs->sock_fd;
    size_t msg_size  = targs->msg_size;
    free(targs);

    printf("[Server-A3] Thread %lu: handling client fd=%d, msg_size=%zu\n",
           (unsigned long)pthread_self(), client_fd, msg_size);

    /* ---- Enable SO_ZEROCOPY on the connected socket ------------------- */
    /*
     * This socket option tells the kernel that subsequent sendmsg() calls
     * with MSG_ZEROCOPY should use the zero-copy path (page pinning +
     * direct DMA) instead of copying data into kernel buffers.
     */
    int zc_flag = 1;
    if (setsockopt(client_fd, SOL_SOCKET, SO_ZEROCOPY,
                   &zc_flag, sizeof(zc_flag)) < 0) {
        perror("[Server-A3] setsockopt SO_ZEROCOPY");
        fprintf(stderr, "[Server-A3] Kernel may not support MSG_ZEROCOPY "
                "(requires Linux >= 4.14)\n");
        close(client_fd);
        return NULL;
    }

    /* ---- Allocate message on the heap (per-thread, private) ----------- */
    message_t msg;
    allocate_message(&msg, msg_size);
    fill_message(&msg, msg_size);

    /* Pre-compute per-field sizes */
    size_t per_field = msg_size / NUM_FIELDS;
    size_t remainder = msg_size % NUM_FIELDS;

    /* ---- Pre-register iovec ------------------------------------------- */
    struct iovec iov[NUM_FIELDS];
    for (int i = 0; i < NUM_FIELDS; i++) {
        iov[i].iov_base = msg.field[i];
        iov[i].iov_len  = per_field + ((i == NUM_FIELDS - 1) ? remainder : 0);
    }

    /* ---- Prepare msghdr (reused across all sends) --------------------- */
    struct msghdr mh;
    memset(&mh, 0, sizeof(mh));
    mh.msg_name       = NULL;
    mh.msg_namelen    = 0;
    mh.msg_iov        = iov;
    mh.msg_iovlen     = NUM_FIELDS;
    mh.msg_control    = NULL;
    mh.msg_controllen = 0;
    mh.msg_flags      = 0;

    /* ---- Per-thread zero-copy tracking -------------------------------- */
    /*
     * pending_zc tracks how many zero-copy sends are "in flight" —
     * i.e., the kernel still has our pages pinned and the NIC hasn't
     * finished DMA.  We drain completions periodically to keep this
     * bounded and avoid exhausting kernel resources (pinned pages,
     * notification queue entries).
     */
    size_t pending_zc       = 0;
    size_t total_bytes_sent = 0;
    size_t total_messages   = 0;
    double start_time       = get_time_us();

    /* Threshold: drain completions when this many are outstanding */
    const size_t ZC_DRAIN_THRESHOLD = 256;

    /* ---- Main send loop ----------------------------------------------- */
    while (g_running) {

        /*
         * =================================================================
         *  ZERO-COPY SEND — sendmsg() with MSG_ZEROCOPY
         * =================================================================
         *
         *  When MSG_ZEROCOPY is set:
         *    1. The kernel does NOT copy data from user-space buffers into
         *       sk_buffs.
         *    2. Instead, it pins the physical pages backing msg.field[0..7]
         *       and creates sk_buff frags pointing to those pages.
         *    3. The NIC DMA engine reads directly from the pinned user
         *       pages into the hardware TX ring.
         *    4. After DMA completes, the kernel unpins the pages and
         *       delivers a completion notification on the error queue.
         *
         *  *** The user→kernel copy is COMPLETELY ELIMINATED. ***
         *
         *  Trade-off: page pinning + completion tracking adds latency for
         *  small messages, so zero-copy is most beneficial for large
         *  payloads where the copy cost would dominate.
         * =================================================================
         */
        ssize_t ret = sendmsg(client_fd, &mh, MSG_ZEROCOPY | MSG_NOSIGNAL);

        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == ENOBUFS) {
                /*
                 * Too many zero-copy sends in flight — the kernel ran out
                 * of notification slots.  Drain completions and retry.
                 */
                drain_completions(client_fd, &pending_zc);
                usleep(100);    /* Brief back-off */
                continue;
            }
            if (errno == EPIPE || errno == ECONNRESET) {
                printf("[Server-A3] Thread %lu: client gone (%s)\n",
                       (unsigned long)pthread_self(), strerror(errno));
            } else {
                perror("[Server-A3] sendmsg MSG_ZEROCOPY");
            }
            break;
        }

        if (ret == 0) {
            printf("[Server-A3] Thread %lu: client disconnected\n",
                   (unsigned long)pthread_self());
            break;
        }

        total_bytes_sent += (size_t)ret;
        pending_zc++;

        if ((size_t)ret == msg_size) {
            total_messages++;
        }

        /*
         * Periodically drain the error queue to process zero-copy
         * completion notifications.  This prevents unbounded growth
         * of pinned pages and kernel notification structures.
         */
        if (pending_zc >= ZC_DRAIN_THRESHOLD) {
            drain_completions(client_fd, &pending_zc);
        }
    }

    /* ---- Drain any remaining completions before cleanup ---------------- */
    /*
     * We must wait for all outstanding zero-copy completions before
     * freeing the message buffers.  Otherwise, the kernel/NIC may still
     * be DMA-ing from pages we're about to free — causing data corruption
     * or a kernel oops.
     */
    int drain_retries = 0;
    while (pending_zc > 0 && drain_retries < 1000) {
        drain_completions(client_fd, &pending_zc);
        if (pending_zc > 0) {
            usleep(1000);   /* 1 ms back-off */
            drain_retries++;
        }
    }

    if (pending_zc > 0) {
        fprintf(stderr, "[Server-A3] Thread %lu: WARNING — %zu completions "
                "still outstanding after drain timeout\n",
                (unsigned long)pthread_self(), pending_zc);
    }

    /* ---- Report per-thread statistics --------------------------------- */
    double elapsed_us = get_time_us() - start_time;
    double elapsed_s  = elapsed_us / 1e6;
    double throughput = (elapsed_s > 0.0)
        ? ((double)total_bytes_sent * 8.0) / (elapsed_s * 1e9)
        : 0.0;

    printf("[Server-A3] Thread %lu: sent %zu msgs (%zu bytes) in %.2f s "
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
        fprintf(stderr, "[Server-A3] Invalid port: %d\n", port);
        return EXIT_FAILURE;
    }
    if (msg_size == 0) {
        fprintf(stderr, "[Server-A3] Message size must be > 0\n");
        return EXIT_FAILURE;
    }

    printf("[Server-A3] Zero-Copy (sendmsg + MSG_ZEROCOPY)\n");
    printf("[Server-A3] Port: %d | Message size: %zu bytes\n", port, msg_size);

    /* ---- Install SIGINT handler --------------------------------------- */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) < 0) {
        perror("[Server-A3] sigaction");
        return EXIT_FAILURE;
    }
    signal(SIGPIPE, SIG_IGN);

    /* ---- Create listening socket -------------------------------------- */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("[Server-A3] socket");
        return EXIT_FAILURE;
    }

    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("[Server-A3] setsockopt SO_REUSEADDR");
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
        perror("[Server-A3] bind");
        close(listen_fd);
        return EXIT_FAILURE;
    }

    /* ---- Listen ------------------------------------------------------- */
    if (listen(listen_fd, BACKLOG) < 0) {
        perror("[Server-A3] listen");
        close(listen_fd);
        return EXIT_FAILURE;
    }

    printf("[Server-A3] Listening on port %d … (Ctrl+C to stop)\n", port);

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
            perror("[Server-A3] accept");
            continue;
        }

        printf("[Server-A3] Accepted connection from %s:%d (fd=%d)\n",
               inet_ntoa(client_addr.sin_addr),
               ntohs(client_addr.sin_port),
               client_fd);

        /* Disable Nagle */
        int flag = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        /* Heap-allocate thread args */
        thread_args_t *targs = (thread_args_t *)malloc(sizeof(thread_args_t));
        if (targs == NULL) {
            perror("[Server-A3] malloc thread_args");
            close(client_fd);
            continue;
        }
        targs->sock_fd      = client_fd;
        targs->msg_size     = msg_size;
        targs->duration_sec = 0;

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_handler, targs) != 0) {
            perror("[Server-A3] pthread_create");
            free(targs);
            close(client_fd);
            continue;
        }
        pthread_detach(tid);
    }

    /* ---- Shutdown ----------------------------------------------------- */
    printf("\n[Server-A3] Shutting down …\n");
    close(listen_fd);

    return EXIT_SUCCESS;
}
