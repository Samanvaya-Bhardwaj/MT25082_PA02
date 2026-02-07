# Roll No: MT25082
# ==============================================================================
# Makefile for PA02 — Network I/O primitives analysis
#
# Builds all six executables (two-copy, one-copy, zero-copy server & client).
# ==============================================================================

CC       = gcc
CFLAGS   = -O2 -Wall -pthread
LDFLAGS  = -pthread

# Common source compiled into every binary
COMMON_SRC = MT25082_common.c
COMMON_HDR = MT25082_common.h

# ---------- Binary names ------------------------------------------------------
A1_SERVER = MT25082_A1_Server
A1_CLIENT = MT25082_A1_Client
A2_SERVER = MT25082_A2_Server
A2_CLIENT = MT25082_A2_Client
A3_SERVER = MT25082_A3_Server
A3_CLIENT = MT25082_A3_Client

ALL_BINS  = $(A1_SERVER) $(A1_CLIENT) \
            $(A2_SERVER) $(A2_CLIENT) \
            $(A3_SERVER) $(A3_CLIENT)

# ---------- Default target ----------------------------------------------------
all: $(ALL_BINS)

# ---------- Part A1: Two-Copy (send / recv) -----------------------------------
$(A1_SERVER): MT25082_Part_A1_Server.c $(COMMON_SRC) $(COMMON_HDR)
	$(CC) $(CFLAGS) -o $@ MT25082_Part_A1_Server.c $(COMMON_SRC) $(LDFLAGS)

$(A1_CLIENT): MT25082_Part_A1_Client.c $(COMMON_SRC) $(COMMON_HDR)
	$(CC) $(CFLAGS) -o $@ MT25082_Part_A1_Client.c $(COMMON_SRC) $(LDFLAGS)

# ---------- Part A2: One-Copy (sendmsg + iovec) ------------------------------
$(A2_SERVER): MT25082_Part_A2_Server.c $(COMMON_SRC) $(COMMON_HDR)
	$(CC) $(CFLAGS) -o $@ MT25082_Part_A2_Server.c $(COMMON_SRC) $(LDFLAGS)

$(A2_CLIENT): MT25082_Part_A2_Client.c $(COMMON_SRC) $(COMMON_HDR)
	$(CC) $(CFLAGS) -o $@ MT25082_Part_A2_Client.c $(COMMON_SRC) $(LDFLAGS)

# ---------- Part A3: Zero-Copy (sendmsg + MSG_ZEROCOPY) ----------------------
$(A3_SERVER): MT25082_Part_A3_Server.c $(COMMON_SRC) $(COMMON_HDR)
	$(CC) $(CFLAGS) -o $@ MT25082_Part_A3_Server.c $(COMMON_SRC) $(LDFLAGS)

$(A3_CLIENT): MT25082_Part_A3_Client.c $(COMMON_SRC) $(COMMON_HDR)
	$(CC) $(CFLAGS) -o $@ MT25082_Part_A3_Client.c $(COMMON_SRC) $(LDFLAGS)

# ---------- Clean -------------------------------------------------------------
clean:
	rm -f $(ALL_BINS)
	rm -f MT25082_perf_*.txt MT25082_client_*.txt MT25082_results.csv

.PHONY: all clean
