/*
*  resfs/libresfs/include/resfs.h
*  SPDX-License-Identifier: MIT
*  Copyright (c) 2026 Andrei Kovalenko
*/

#ifndef RESFS_H
#define RESFS_H

#include <stdint.h>
#include <stddef.h>

#define BH_SIG "RESFS PARTITION "
#define ROOT_SIG "RESFS ROOT "
#define EOP_TAIL_TEXT "END OF RESFS PARTITION"
#define EOP_SIG "ResFSEOP"
#define SMI_SIG "ResFSSMI"
#define DLI_SIG "ResFSDLI"
#define SNAP_SIG "ResFSSNP"
#define SEG_SIG "ResFSSEG"
#define SEG_END_SIG "ResFSEND"
#define WIR_SIG "ResFSWIR"
#define EOP_TAIL "END OF RESFS PARTITION"

#define FEAT_ENCRYPTION (1u << 0)
#define FEAT_COMPRESSION (1u << 1)
#define FEAT_SNAPSHOTS (1u << 2)
#define FEAT_SPARSE (1u << 3)
#define FEAT_ACL (1u << 4)
#define FEAT_XATTR (1u << 5)

#define __packed __attribute__((packed))

struct resfs_bh {
	uint8_t bh_sig[16];
	uint32_t version;
	uint32_t block_size;
	uint8_t fs_uuid[16];
	uint8_t label_len;
	uint8_t fs_label[255];
	uint32_t feature_flags;
	uint64_t wir_start;
	uint64_t wir_size;
	uint64_t ir1_start;
	uint64_t ir2_start;
	uint64_t ir3_start;
	uint64_t ir_size;
	uint64_t snap_start;
	uint64_t snap_size;
	uint64_t data1_start;
	uint64_t data2_start;
	uint64_t start_of_partition_lba;
	uint64_t partition_size;
	uint8_t blake3_hash[32];
	uint8_t reserved[3668];
} __packed;

_Static_assert(sizeof(struct resfs_bh) == 4096, "resfs_bh must be 4096 bytes");

struct resfs_eop {
	uint8_t eop_sig[8];
	uint32_t version;
	uint32_t block_size;
	uint8_t fs_uuid[16];
	uint8_t label_len;
	uint8_t fs_label[255];
	uint32_t feature_flags;
	uint64_t wir_start;
	uint64_t wir_size;
	uint64_t ir1_start;
	uint64_t ir2_start;
	uint64_t ir3_start;
	uint64_t ir_size;
	uint64_t snap_start;
	uint64_t snap_size;
	uint64_t data1_start;
	uint64_t data2_start;
	uint64_t start_of_partition_lba;
	uint64_t partition_size;
	uint8_t blake3_hash[32];
	uint8_t reserved[3654];
	uint8_t eop_tail[22];
} __packed;

_Static_assert(sizeof(struct resfs_eop) == 4096, "resfs_eop must be 4096 bytes");

struct resfs_wir_h {
	uint8_t wir_sig[8];
	uint32_t reserved1;
	uint64_t generation;
	uint64_t entry_count;
	uint64_t entry_size;
	uint64_t data_offset;
	uint8_t blake3_hash[32];
	uint8_t reserved2[4020];
} __packed;

_Static_assert(sizeof(struct resfs_wir_h) == 4096, "resfs_wir_h must be 4096 bytes");

#endif /* RESFS_H */