#  resfs/Makefile
#  SPDX-License-Identifier: MIT
#  Copyright (c) 2026 Andrei Kovalenko

CC = gcc
CFLAGS = -Ilibresfs/include -Ilibresfs/vendor/blake3 -Wall -Wextra -g

LIB_SRCS = $(wildcard libresfs/src/*.c) $(wildcard libresfs/vendor/blake3/*.c)
LIB_OBJS = $(LIB_SRCS:.c=.o)

TOOLS = tools/mkfs tools/mount tools/verify tools/recover tools/snap tools/export tools/import tools/visualize

all: libresfs.a $(TOOLS)

libresfs.a: $(LIB_OBJS)
	ar rcs $@ $^

tools/%: tools/%.c libresfs.a
	$(CC) $(CFLAGS) $< -L. -lresfs -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

test: libresfs/src/test.c
	$(CC) $(CFLAGS) $< -o /dev/null

clean:
	rm -f $(LIB_OBJS) libresfs.a $(TOOLS)

.PHONY: all test clean