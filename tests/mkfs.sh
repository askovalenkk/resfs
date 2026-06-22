#!/bin/bash
#  resfs/tests/mkfs.sh
#  SPDX-License-Identifier: MIT
#  Copyright (c) 2026 Andrei Kovalenko

set -e
chmod +x tests/mkfs.sh

make
dd if=/dev/zero of=disk.img bs=1M count=64
./tools/mkfs disk.img
./tools/verify disk.img