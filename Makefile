#  resfs/Makefile
#  SPDX-License-Identifier: MIT
#  Copyright (c) 2026 Andrei Kovalenko

CC = gcc
AR = ar

ARCH ?= $(shell uname -m)

CFLAGS = -Ilibresfs/include -Ilibresfs/vendor/blake3 -Itools/include \
         -Wall -Wextra -Wpedantic -g

OBJDIR = build

LIBRESFS_SRCS = $(wildcard libresfs/src/*.c)

BLAKE3_SRCS = \
 libresfs/vendor/blake3/blake3.c \
 libresfs/vendor/blake3/blake3_portable.c

ifeq ($(ARCH),x86_64)
BLAKE3_SRCS += \
 libresfs/vendor/blake3/blake3_dispatch.c \
 libresfs/vendor/blake3/blake3_sse2.c \
 libresfs/vendor/blake3/blake3_sse41.c \
 libresfs/vendor/blake3/blake3_avx2.c \
 libresfs/vendor/blake3/blake3_avx512.c
endif

ifeq ($(ARCH),aarch64)
BLAKE3_SRCS += \
 libresfs/vendor/blake3/blake3_dispatch.c \
 libresfs/vendor/blake3/blake3_neon.c
endif

LIB_SRCS = $(LIBRESFS_SRCS) $(BLAKE3_SRCS)
LIB_OBJS = $(patsubst %.c,$(OBJDIR)/%.o,$(LIB_SRCS))

TOOLS = tools/mkfs tools/verify tools/recover tools/snap tools/export tools/import tools/visualize

all: libresfs.a $(TOOLS)

libresfs.a: $(LIB_OBJS)
 $(AR) rcs $@ $^

$(OBJDIR)/%.o: %.c
 mkdir -p $(dir $@)
 $(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/libresfs/vendor/blake3/blake3_sse2.o: CFLAGS += -msse2
$(OBJDIR)/libresfs/vendor/blake3/blake3_sse41.o: CFLAGS += -msse4.1
$(OBJDIR)/libresfs/vendor/blake3/blake3_avx2.o: CFLAGS += -mavx2
$(OBJDIR)/libresfs/vendor/blake3/blake3_avx512.o: CFLAGS += -mavx512f -mavx512vl

tools/%: tools/%.c libresfs.a
 $(CC) $(CFLAGS) $< -L. -lresfs -o $@

clean:
 rm -rf $(OBJDIR) libresfs.a $(TOOLS)

install: $(TOOLS)
 install -m 755 tools/mkfs /usr/local/bin/mkfs.resfs
 install -m 755 tools/verify /usr/local/bin/resfs-verify
 install -m 755 tools/recover /usr/local/bin/resfs-recover
 install -m 755 tools/snap /usr/local/bin/resfs-snap
 install -m 755 tools/export /usr/local/bin/resfs-export
 install -m 755 tools/import /usr/local/bin/resfs-import
 install -m 755 tools/visualize /usr/local/bin/resfs-visualize

.PHONY: all clean install

-include local.mk