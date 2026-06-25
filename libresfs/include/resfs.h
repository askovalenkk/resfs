/*
*  resfs/libresfs/include/resfs.h
*  SPDX-License-Identifier: MIT
*  Copyright (c) 2026 Andrei Kovalenko
*/

#ifndef RESFS_H
#define RESFS_H

#include <stdint.h>
#include <stddef.h>

// ResFS signatures
#define BH_SIG "RESFS PARTITION "
#define ROOT_SIG "RESFS ROOT"
#define EOP_SIG "ResFSEOP"
#define SMI_SIG "ResFSSMI"
#define DLI_SIG "ResFSDLI"
#define SNAP_SIG "ResFSSNP"
#define SEG_SIG "ResFSSEG"
#define SEG_END_SIG "ResFSEND"
#define WIA_SIG "ResFSWIA"
#define EOP_TAIL "END OF RESFS PARTITION"

// BH feature flags
#define FEAT_ENCRYPTION (1u << 0)
#define FEAT_COMPRESSION (1u << 1)
#define FEAT_SNAPSHOTS (1u << 2)
#define FEAT_SPARSE (1u << 3)
#define FEAT_ACL (1u << 4)
#define FEAT_XATTR (1u << 5)

// SEG flags
#define IS_FIRST_SEG (1u << 0)
#define IS_LAST_SEG (1u << 1)
#define IS_COMPRESSED (1u << 2)
#define IS_ENCRYPTED (1u << 3)
#define IS_INLINE (1u << 4)
#define IS_DELETED (1u << 5)
#define IS_COMMITTED (1u << 6)
#define IS_DIRECTORY (1u << 7)
#define IS_SYMLINK (1u << 8)
#define IS_DEVICE_FILE (1u << 9)
#define IS_SPARSE_SEG (1u << 10)
#define IS_XATTR_SEG (1u << 11)
#define IS_ACL_SEG (1u << 12)
#define IS_SNAPSHOT_FILE (1u << 13)
#define EXT_OVERFLOW (1u << 14)

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

struct resfs_wia_h {
	uint8_t wia_sig[8];
	uint32_t reserved1;
	uint64_t generation;
	uint64_t entry_count;
	uint64_t capacity;
	uint64_t data_offset;
	uint8_t blake3_hash[32];
	uint8_t reserved2[4020];
} __packed;

_Static_assert(sizeof(struct resfs_wia_h) == 4096, "resfs_wia_h must be 4096 bytes");

struct resfs_wia_entry {
	uint64_t file_id;
	uint8_t operation;
	uint8_t reserved[3];
	uint32_t ext_count;
	uint8_t extents[20 * ext_count];
} __packed;

struct resfs_sr_h {
	uint8_t sr_sig[8];
	uint64_t snapshot_counter;
	uint64_t entry_count;
	uint64_t live_count;
	uint8_t reserved[4064];
} __packed;

_Static_assert(sizeof(struct resfs_sr_h) == 4096, "resfs_sr_h must be 4096 bytes");

struct resfs_sr_entry {
	uint64_t snapshot_id;
	uint64_t snap_file_id;
	uint64_t created_at;
	uint8_t live;
	uint8_t reserved[3];
	uint8_t blake3_hash[32];
} __packed;

struct resfs_smi_h {
	uint8_t smi_sig[8];
	uint64_t reserved1;
	uint64_t generation;
	uint64_t file_count;
	uint64_t used_blocks;
	uint64_t last_mount;
	uint64_t entry_count;
	uint8_t blake3_hash[32];
	uint8_t reserved2[4008];
} __packed;

_Static_assert(sizeof(struct resfs_smi_h) == 4096, "resfs_smi_h must be 4096 bytes");	

struct resfs_smi_entry {
	uint64_t file_id;
	uint64_t seg0_lba;
} __packed;

struct resfs_dli_h {
	uint8_t dli_sig[8];
	uint64_t reserved1;
	uint64_t generation;
	uint64_t entry_count;
	uint64_t data_offset;
	uint8_t blake3_hash[32];
	uint8_t reserved2[4024];
} __packed;

_Static_assert(sizeof(struct resfs_dli_h) == 4096, "resfs_dli_h must be 4096 bytes");

struct resfs_dli_entry {
	uint64_t name_hash;
	uint64_t file_id;
	uint64_t parent_dir_id;
} __packed;

struct resfs_seg0 {
	uint8_t seg_sig[8];
	uint64_t file_id;
	uint32_t seg_index;
	uint32_t flags;
	uint32_t data_len;
	uint32_t reserved1;
	uint64_t file_size;
	uint64_t snapshot_id;
	uint64_t created_at;
	uint8_t blake3_hash[32];
	uint8_t filename_len;
	uint8_t filename[255];
	uint32_t permissions;
	uint32_t reserved2;
	uint64_t generation;
	uint64_t modified_at;
	uint64_t owner_uid;
	uint64_t owner_gid;
	uint64_t hardlink_id;
	uint8_t data_region[3688];
	uint8_t seg_end_sig[8];
	uint64_t file_id_end;
	uint32_t seg_index_end;
	uint32_t reserved3;
} __packed;

_Static_assert(sizeof(struct resfs_seg0) == 4096, "resfs_seg0 must be 4096 bytes");

struct resfs_seg {
	uint8_t seg_sig[8];
	uint64_t file_id;
	uint32_t seg_index;
	uint32_t flags;
	uint32_t data_len;
	uint32_t reserved1;
	uint64_t file_size;
	uint64_t snapshot_id;
	uint64_t created_at;
	uint8_t blake3_hash[32];
	uint8_t data_region[4000];
	uint8_t seg_end_sig[8];
	uint64_t file_id_end;
	uint32_t seg_index_end;
	uint32_t reserved2;
} __packed;

_Static_assert(sizeof(struct resfs_seg) == 4096, "resfs_seg must be 4096 bytes");

#endif /* RESFS_H */