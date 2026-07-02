#  resfs/Makefile
#  SPDX-License-Identifier: MIT
#  Copyright (c) 2026 Andrei Kovalenko

CC = gcc
CFLAGS = -Ilibresfs/include -Ilibresfs/vendor/blake3 -Itools/include -Wall -Wextra -Wpedantic -g 
OBJDIR = build

LIB_SRCS = $(wildcard libresfs/src/*.c) $(wildcard libresfs/vendor/blake3/*.c)
LIB_OBJS = $(patsubst %.c,$(OBJDIR)/%.o,$(LIB_SRCS))

TOOLS = tools/mkfs tools/verify tools/recover tools/snap tools/export tools/import tools/visualize

all: libresfs.a $(TOOLS)

libresfs.a: $(LIB_OBJS)
	ar rcs $@ $^

$(OBJDIR)/%.o: %.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

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