/*
*  resfs/tools/mkfs.c
*  SPDX-License-Identifier: MIT
*  Copyright (c) 2026 Andrei Kovalenko
*/

#include "tools.h"

struct mkfs_args {
	char *label;
	char *device;
	int size_mb;
	int force; /* 1 = force */
	int dev_type; /* 0 = disk image; 1 = block device */
	int op_type; /* 0 = create new disk image; 1 = format*/
	double sr_percent;
};

struct dev_params {
	char parent_dev[PATH_MAX];
	int fd;
	int pdev_fd;
	int partition_number; 
	int is_disk_part; /* 0 = whole disk; 1 = disk partition */
	int no_parent_dev_access; /* 0 by default; 1 = no access to parent device */
	uint64_t size_bytes;
	uint64_t parent_dev_size_bytes;
	uint64_t part_start_lba; 
	uint32_t logical_sector_size; /* only 4Kn & 512e supported */
	uint32_t physical_sector_size; /* only 4KB physical sector size supported */
};

struct mbr_partition_entry {
	uint8_t boot_indicator;
	uint8_t starting_chs[3]; /* 0x000200 */
	uint8_t os_type; /* 0xEE (GPT Protective) */
	uint8_t ending_chs[3]; /* 0xFFFFFF */
	uint32_t starting_lba; /* 0x00000001 (LBA of GPT Header) */
	uint32_t size_in_lba; /* 0xFFFFFFFF */
} __packed;

_Static_assert(sizeof(struct mbr_partition_entry) == 16, "MBR Partition Entry size must be 16 bytes");

struct uuid { /* 128-bit RFC 4122 mixed-endian */
	uint32_t data1;
	uint16_t data2;
	uint16_t data3;
	uint8_t data4[8];
} __packed; 

struct protective_mbr {
	uint8_t reserved[446];
	struct mbr_partition_entry entry;
	uint8_t unused_entries[3][16];
	uint8_t boot_signature[2]; /* 0x55AA */
} __packed;

_Static_assert(sizeof(struct protective_mbr) == 512, "Protective MBR size must be 512 bytes");

struct gpt_header {
	uint8_t signature[8]; /* "EFI PART" */
	uint32_t revision; /* 0x00010000 */
	uint32_t header_size; /* Header size in bytes (must be between 92 and logical block size) */
	uint32_t header_crc32;
	uint32_t reserved;
	uint64_t my_lba; /* LBA1 */
	uint64_t alternate_lba; /* LBA of the Secondary GPT Header */
	uint64_t first_usable_lba;
	uint64_t last_usable_lba;
	struct uuid disk_guid;
	uint64_t partition_entry_lba;
	uint32_t partition_entry_count;
	uint32_t size_of_partition_entry;
	uint32_t partition_entry_array_crc32;
} __packed;

_Static_assert(sizeof(struct gpt_header) == 92, "GPT Header size must be 92 bytes");

struct gpt_partition_entry {
	struct uuid partition_type_guid; /* ResFS official GUID */
	struct uuid unique_partition_guid; /* UUIDv4 */
	uint64_t starting_lba;
	uint64_t ending_lba;
	uint64_t attributes; /* Not used in v0.99.1 */
	uint8_t partition_name[72]; /* UTF-16LE Partition name (default "ResFS Disk Partition") */
} __packed;

_Static_assert(sizeof(struct gpt_partition_entry) == 128, "GPT Partition Entry size must be 128 bytes");

struct gpt_partition_entries {
	struct gpt_partition_entry entry1;
	struct gpt_partition_entry unused_entries[127];
} __packed;

static uint32_t crc32_table[256];

static void crc32_init(void) 
{
	for (uint32_t i = 0; i < 256; i++) {
        	uint32_t c = i;
        	for (int j = 0; j < 8; j++)
            		c = (c >> 1) ^ (c & 1 ? 0xEDB88320 : 0);
        	crc32_table[i] = c;
	}
}

static uint32_t crc32(const void *buf, size_t len) 
{
	const uint8_t *p = buf;
	uint32_t c = 0xFFFFFFFF;
    	while (len--)
		c = (c >> 8) ^ crc32_table[(c ^ *p++) & 0xFF];
    	return c ^ 0xFFFFFFFF;
}

static void blake3_hash(const void *buf, size_t len, uint8_t out[32])
{
	blake3_hasher hasher;

	blake3_hasher_init(&hasher);
	blake3_hasher_update(&hasher, buf, len);
	blake3_hasher_finalize(&hasher, out, 32);
}

static uint64_t blake3_hash_name(const char *name)
{
	blake3_hasher hasher;
	uint8_t out[8];
	blake3_hasher_init(&hasher);
	blake3_hasher_update(&hasher, name, strlen(name));
	blake3_hasher_finalize(&hasher, out, 8);
	uint64_t hash;
	memcpy(&hash, out, 8);
	return hash;
}

static uint64_t align_up(uint64_t value, uint64_t alignment) 
{
	return ((value + alignment - 1) / alignment) * alignment;
}

static int generate_uuidv4(struct uuid *uuid)
{
	if (getrandom(uuid, sizeof(*uuid), 0) != sizeof(*uuid)) {
		return 1;
	}
	uuid->data3 = (uuid->data3 & 0x0FFF) | 0x4000;
	uuid->data4[0] = (uuid->data4[0] & 0x3F) | 0x80;
	return 0;
}

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void ascii_to_utf16le(uint8_t dst[72], const char *src)
{
    while (*src) {
        *dst++ = *src++;
        *dst++ = 0;
    }
}

static uint64_t max(uint64_t a, uint64_t b)
{
	return a > b ? a : b;
}

int help(void)
{
	printf("\n");
	printf("Usage: mkfs.resfs [options] <device>\n\n");
	printf("Options:\n");
	printf("  -h            Show this help\n");
	printf("  -l LABEL      Set partition label (default: none)\n");
	printf("  -s PERCENT    Set custom Snapshot Region size (default: auto)\n");
	printf("  -c SIZE       Creates a disk partition with size set (in MB)\n");
	printf("  -f            Force formatting without confirmation\n\n");
	printf("  <device>      Path to the device or the filename of the disk image to be used\n");

	return 0;
}

int gdisk_manual(void)
{
	printf("\n");
	printf("GPT is not altered due to no access to parental block device.\n");
	printf("Please update Partition Type GUID using gdisk or other utility.\n\n");
	printf("ResFS Partition Type GUID: 52455346-494C-4553-5953-54454D2F414B.\n");

	return 0;
}

int parse_args(int argc, char *argv[], struct mkfs_args *args)
{
	int opt;
	while ((opt = getopt(argc, argv, ":hl:s:c:f")) != -1) {
		switch (opt) {
		case 'h': 
			help();
			return 1;
		case 'l':
			if (strlen(optarg) <= 255) {
				args->label = optarg;
			}
			else {
				fprintf(stderr, "mkfs.resfs: error: label too long (max: 255 bytes)\n");
				return ERR_INVALID;
			}
			break;
		case 's': {
			double pct = strtod(optarg, NULL);
			if (pct >= 0.1 && pct <= 3.0) {
				args->sr_percent = pct;
			}
			else {
				fprintf(stderr, "mkfs.resfs: error: invalid Snapshot Region size (must be between 0.1%% and 3%%)\n");
				return ERR_INVALID;
			}
			break;
		}
		case 'c': {
			long size = strtol(optarg, NULL, 10);
			if (size >= RESFS_MIN_SIZE_MB) {
				args->size_mb = size;
			}
			else {
				fprintf(stderr, "mkfs.resfs: error: invalid disk image size (min: 16MB)\n");
				return ERR_INVALID;
			}
			break;
		}
		case 'f':
			args->force = 1;
			break;
		case ':':
    			fprintf(stderr, "mkfs.resfs: error: option -%c requires an argument\n", optopt);
    			return ERR_INVALID;
		case '?':
    			fprintf(stderr, "mkfs.resfs: error: unknown option -%c\n", optopt);
    			return ERR_INVALID;
		default:
			fprintf(stderr, "mkfs.resfs: error: invalid input\n");
    			return ERR_INVALID;
		}
	}
	if (optind < argc) {
		args->device = argv[optind];
		return 0;
	}
	else if (argc == 1) {
		help();
		return 1;
	}
	else {
		fprintf(stderr, "mkfs.resfs: error: no device file specified\n");
		return ERR_INVALID;
	}
}

int check_disk (struct mkfs_args *args)
{
	int ex = access(args->device, F_OK);
	int rw = access(args->device, R_OK | W_OK);
	if (ex == 0) {
		args->op_type = 1;
		struct stat st;
		if (stat(args->device, &st) != 0) {
			fprintf(stderr, "mkfs.resfs: error: no access to the block device\n");
			return ERR_INVALID;
		}
		if (S_ISBLK(st.st_mode)) {
			if (rw == 0) {
				if (args->size_mb) {
					fprintf(stderr, "mkfs.resfs: error: -c is not applicable to block devices\n");
					return ERR_INVALID;
				}
				else {
					args->dev_type = 1;
					args->op_type = 1;
					return 0;
				}
			}
			else {
				fprintf(stderr, "mkfs.resfs: error: no access to the block device\n");
				return ERR_INVALID;
			}
		}
		else if (S_ISREG(st.st_mode)) {
			if (rw == 0) {
				if (args->size_mb) {
					fprintf(stderr, "mkfs.resfs: error: -c is not applicable to existing disk image files\n");
					return ERR_INVALID;
				}
				return 0;
			}
			else {
				fprintf(stderr, "mkfs.resfs: error: no access to the disk image file\n");
				return ERR_INVALID;
			}
		}
		else {
			fprintf(stderr, "mkfs.resfs: error: invalid path\n");
			return ERR_INVALID;
		}
	}
	else {
		char *path_copy = strdup(args->device);
		if (!path_copy) {
			fprintf(stderr, "mkfs.resfs: error: unknown error\n");
			return ERR_INVALID;
		}
		char *dir = dirname(path_copy);
		int w = access(dir, W_OK);
		if (strncmp(dir, "/dev", 4) == 0 && (dir[4] == '/' || dir[4] == '\0')) {
			fprintf(stderr, "mkfs.resfs: error: cannot create disk image in /dev\n");
			free(path_copy);
			return ERR_INVALID;
		}
		else if (strncmp(dir, "/sys", 4) == 0 && (dir[4] == '/' || dir[4] == '\0')) {
			fprintf(stderr, "mkfs.resfs: error: cannot create disk image in /sys\n");
			free(path_copy);
			return ERR_INVALID;
		}
		else if (strncmp(dir, "/proc", 5) == 0 && (dir[5] == '/' || dir[5] == '\0')) {
			fprintf(stderr, "mkfs.resfs: error: cannot create disk image in /proc\n");
			free(path_copy);
			return ERR_INVALID;
		}
		else if (strncmp(dir, "/boot", 5) == 0 && (dir[5] == '/' || dir[5] == '\0')) {
			fprintf(stderr, "mkfs.resfs: error: cannot create disk image in /boot\n");
			free(path_copy);
			return ERR_INVALID;
		}
		else if (strncmp(dir, "/run", 4) == 0 && (dir[4] == '/' || dir[4] == '\0')) {
			fprintf(stderr, "mkfs.resfs: error: cannot create disk image in /run\n");
			free(path_copy);
			return ERR_INVALID;
		}
		if (w == 0) {
			if (args->size_mb != 0) {
				free(path_copy);
				return 0;
			}
			else {
				fprintf(stderr, "mkfs.resfs: error: device does not exist and no size specified (-c)\n");
				free(path_copy);
				return ERR_INVALID;
			}
		}
		else {
			fprintf(stderr, "mkfs.resfs: error: invalid path\n");
			free(path_copy);
			return ERR_INVALID;
		}
	}
	
}

int check_disk_type(struct mkfs_args *args, struct dev_params *params)
{
	char *tmp = strdup(args->device);
	if (!tmp) {
		fprintf(stderr, "mkfs.resfs: error: unknown error\n");
		return ERR_INVALID;
	}
	char *bn = basename(tmp);
	char path[PATH_MAX];
	char disk_path[PATH_MAX];
	char rp[PATH_MAX];
	char parent_dev[PATH_MAX];

	snprintf(path, sizeof(path), "/sys/class/block/%s/partition", bn);
	snprintf(disk_path, sizeof(disk_path), "/sys/class/block/%s", bn);

	if (access(path, F_OK) == 0) {
		params->is_disk_part = 1;
		FILE *f = fopen(path, "r");
		if (f) {
			fscanf(f, "%d", &params->partition_number);
			fclose(f);
		}
		if (realpath(disk_path, rp)) {
			char *pdir = dirname(rp);
			char *pdisk = basename(pdir);
			snprintf(parent_dev, sizeof(parent_dev), "/dev/%s", pdisk);
			free(tmp);
			if (access(parent_dev, W_OK | R_OK) == 0) {
				snprintf(params->parent_dev,sizeof(params->parent_dev), "%s", parent_dev);
				return 0;
			}
			else {
				params->no_parent_dev_access = 1;
				return 0;
			}
		}
		else {
			fprintf(stderr, "mkfs.resfs: error: failed to resolve device path\n");
			free(tmp);
			return ERR_INVALID;
		}
	}
	free(tmp);
	return 0;
}

int confirm(struct mkfs_args *args, struct dev_params *params) {
	if (args->force) {
		return 0;
	}
	else if (args->dev_type == 1) {
		if (params->is_disk_part) {
			printf("You are going to format disk partition: %s!\n", args->device);
			printf("This operation will erase all data on the partition. Continue? [Y/n]: ");
			char ans = getchar();
			int c;
			while ((c = getchar()) != '\n' && c != EOF);
			if (ans == 'Y' || ans == 'y') {
				printf("\n");
				return 0;
			}
			else {
				printf("\n");
				return 1;
			}
		}
		else {
			printf("You are going to format block device: %s!\n", args->device);
			printf("This operation will erase all data on the disk. Continue? [Y/n]: ");
			char ans = getchar();
			int c;
			while ((c = getchar()) != '\n' && c != EOF);
			if (ans == 'Y' || ans == 'y') {
				printf("\n");
				return 0;
			}
			else {
				printf("\n");
				return 1;
			}
		}
	}
	else if (args->dev_type == 0) {
		if (args->op_type) {
			printf("You are going to format existing disk image: %s!\n", args->device);
			printf("This operation will erase all data on the disk image. Continue? [Y/n]: ");
			char ans = getchar();
			int c;
			while ((c = getchar()) != '\n' && c != EOF);
			if (ans == 'Y' || ans == 'y') {
				printf("\n");
				return 0;
			}
			else {
				printf("\n");
				return 1;
			}
		}
		else {
			printf("You are going to create disk image %s.\n", args->device);
			printf("Continue? [Y/n]: ");
			char ans = getchar();
			int c;
			while ((c = getchar()) != '\n' && c != EOF);
			if (ans == 'Y' || ans == 'y') {
				printf("\n");
				return 0;
			}
			else {
				printf("\n");
				return 1;
			}
		}
	}
	else {
		fprintf(stderr, "mkfs.resfs: error: unknown error\n");
		return ERR_INVALID;
	}
}

int get_params(struct mkfs_args *args, struct dev_params *params)
{
	if (args->dev_type) {
		int fd = open(args->device, O_RDWR);
		if (fd == -1) {
			fprintf(stderr, "mkfs.resfs: error: failed to open block device\n");
			return ERR_INVALID;
		}
		if (ioctl(fd, BLKGETSIZE64, &params->size_bytes) == -1) {
			fprintf(stderr, "mkfs.resfs: error: failed to determine block device size\n");
			close(fd);
			return ERR_INVALID;
		}
		if (ioctl(fd, BLKSSZGET, &params->logical_sector_size) == -1) {
			fprintf(stderr, "mkfs.resfs: error: failed to determine logical sector size\n");
			close(fd);
			return ERR_INVALID;
		}
		if (ioctl(fd, BLKPBSZGET, &params->physical_sector_size) == -1) {
			fprintf(stderr, "mkfs.resfs: error: failed to determine physical sector size\n");
			close(fd);
			return ERR_INVALID;
		}
		/* if (params->physical_sector_size != 4096) {
			fprintf(stderr, "mkfs.resfs: error: unsupported physical block size (must be 4KB)\n");
			close(fd);
			return ERR_INVALID;
		} */ 
		else {
			params->fd = fd;
			return 0;
		}
	}
	else {
		int fd = open(args->device, O_RDWR);
		if (fd == -1) {
			fprintf(stderr, "mkfs.resfs: error: failed to open disk image\n");
			return ERR_INVALID;
		}
		struct stat st;
		if (fstat(fd, &st) == -1) {
			fprintf(stderr, "mkfs.resfs: error: failed to determine disk image size\n");
			close(fd);
			return ERR_INVALID;
		}
		params->size_bytes = st.st_size;
		params->fd = fd;
		params->logical_sector_size = 4096;
		params->physical_sector_size = 4096;
		return 0;
	}
}

int create_image(struct mkfs_args *args, struct dev_params *params)
{
	int fd = open(args->device, O_RDWR | O_CREAT, 0644);
	if (fd == -1) {
		fprintf(stderr, "mkfs.resfs: error: failed to create a disk image\n");
		return ERR_INVALID;
	}
	params->size_bytes = (uint64_t)args->size_mb * 1024 * 1024;
	if (fallocate(fd, 0, 0, params->size_bytes) == -1) {
		fprintf(stderr, "mkfs.resfs: error: failed to create a disk image\n");
		close(fd);
		unlink(args->device);
		return ERR_INVALID;
	}
	params->fd = fd;
	params->logical_sector_size = 4096;
	params->physical_sector_size = 4096;
	return 0;
}

int get_pdev_params(struct mkfs_args *args, struct dev_params *params)
{
	char path[PATH_MAX];
	char *tmp = strdup(args->device);
	if (!tmp) {
		fprintf(stderr, "mkfs.resfs: error: unknown error\n");
		return ERR_INVALID;
	}
	char *bn = basename(tmp);
	snprintf(path, sizeof(path), "/sys/class/block/%s/start", bn);
	FILE *f = fopen(path, "r");
	if (f) {
		fscanf(f, "%lu", &params->part_start_lba);
		fclose(f);
	}
	if (!params->part_start_lba) {
		fprintf(stderr, "mkfs.resfs: error: failed to determine block device size\n");
		free(tmp);
		return ERR_INVALID;
	}
	params->pdev_fd = open(params->parent_dev, O_RDWR);
	if (params->pdev_fd == -1) {
		fprintf(stderr, "mkfs.resfs: error: failed to open block device\n");
		free(tmp);
		return ERR_INVALID;
	}
	if (ioctl(params->pdev_fd, BLKGETSIZE64, &params->parent_dev_size_bytes) == -1) {
		fprintf(stderr, "mkfs.resfs: error: failed to determine partition start LBA\n");
		close(params->pdev_fd);
		free(tmp);
		return ERR_INVALID;
	}
	free(tmp);
	return 0;
}

int calc_layout(struct dev_params *params, struct resfs_bh *bh)
{
	bh->block_size = RESFS_BLOCK_SIZE;
	bh->logical_sector_size = params->logical_sector_size;

	if (params->is_disk_part) {
		bh->start_of_partition = params->part_start_lba;
		bh->total_blocks = params->size_bytes / 4096;
		bh->partition_size = bh->total_blocks * 4096 / bh->logical_sector_size;
	} 
	else {
		uint64_t alignment_lba = 1048576 / params->logical_sector_size;
		bh->start_of_partition = align_up(2 + 16384 / params->logical_sector_size, alignment_lba);
		uint64_t secondary_reserved_lba = 1 + 16384 / params->logical_sector_size;
		uint64_t last_usable_lba = (params->size_bytes / params->logical_sector_size - 1) - secondary_reserved_lba;
		uint64_t usable_lba_count = last_usable_lba - bh->start_of_partition + 1;
		uint64_t usable_bytes = usable_lba_count * params->logical_sector_size;
		bh->total_blocks = usable_bytes / 4096;
		bh->partition_size = bh->total_blocks * 4096 / bh->logical_sector_size;
	}

	bh->wia_size = max(RESFS_MIN_WIA_BLOCKS, bh->total_blocks / 1000);
	bh->sr_size = max(RESFS_MIN_SR_BLOCKS, bh->total_blocks * 2 / 1000);
	bh->ir_size = max(RESFS_MIN_IR_BLOCKS, bh->total_blocks * 2 / 1000);

	bh->wia_start = 1;
	bh->sr_start = bh->wia_start + bh->wia_size;
	bh->ir1_start = bh->sr_start + bh->sr_size;
	bh->ir2_start = bh->total_blocks / 2;
	bh->ir3_start = bh->total_blocks - bh->ir_size - 1;

	bh->data1_start = bh->ir1_start + bh->ir_size;
	bh->data2_start = bh->ir2_start + bh->ir_size;
	return 0;
}

int write_lba(int fd, uint64_t lba, struct dev_params *params, const void *buf, size_t len)
{
	if (len > params->logical_sector_size) {
		return 1;
	}
	void *sector = calloc(1, params->logical_sector_size);
	if (!sector) {
		return 1;
	}
	memcpy(sector, buf, len);
	off_t offset = lba * params->logical_sector_size;
	ssize_t size = pwrite(fd, sector, params->logical_sector_size, offset);
	free(sector);
	if (size != (ssize_t)params->logical_sector_size) {
		return 1;
	}
	return 0;
}	

int write_segment(struct resfs_bh *bh, struct dev_params *params, uint64_t start, const void *buf, size_t len) 
{
	if (len > RESFS_BLOCK_SIZE) {
		return 1;
	}
	void *segment = calloc(1, RESFS_BLOCK_SIZE);
	if (!segment) {
		return 1;
	}
	memcpy(segment, buf, len);
	uint64_t seg_size_lba = RESFS_BLOCK_SIZE / params->logical_sector_size;
	uint64_t lba = bh->start_of_partition + start * seg_size_lba;
	off_t offset = lba * params->logical_sector_size;
	ssize_t size = pwrite(params->fd, segment, RESFS_BLOCK_SIZE, offset);
	free(segment);
	if (size != (ssize_t)RESFS_BLOCK_SIZE) {
		return 1;
	}
	return 0;
}

int build_protective_mbr(struct protective_mbr *mbr, struct dev_params *params) 
{
	mbr->entry.starting_chs[0] = 0x00;
	mbr->entry.starting_chs[1] = 0x02;
	mbr->entry.starting_chs[2] = 0x00;
	mbr->entry.os_type = 0xEE;
	mbr->entry.ending_chs[0] = 0xFF;
	mbr->entry.ending_chs[1] = 0xFF;
	mbr->entry.ending_chs[2] = 0xFF;
	mbr->entry.starting_lba = 0x00000001;

	uint64_t disk_lba_count = params->size_bytes / params->logical_sector_size;
	if (disk_lba_count - 1 > 0xFFFFFFFF) {
		mbr->entry.size_in_lba = 0xFFFFFFFF;
	}
	else {
		mbr->entry.size_in_lba = disk_lba_count - 1;
	}

	mbr->boot_signature[0] = 0x55;
	mbr->boot_signature[1] = 0xAA;
	return 0; 
}

int build_gpt_partition_entry(struct gpt_partition_entry *entry, struct uuid *part_guid, struct uuid *part_uuid, struct resfs_bh *bh)
{
        entry->partition_type_guid = *part_guid;
        entry->unique_partition_guid = *part_uuid;
        entry->starting_lba = bh->start_of_partition;
        entry->ending_lba = bh->start_of_partition + bh->partition_size - 1;
	ascii_to_utf16le(entry->partition_name, "ResFS Disk Partition");
	return 0;
}

int build_primary_gpt_header(struct gpt_header *gpt_h, struct dev_params *params, struct resfs_bh *bh, struct uuid *disk_guid)
{
	memcpy(gpt_h->signature, "EFI PART", 8);
	gpt_h->revision = 0x00010000;
	gpt_h->header_size = sizeof(struct gpt_header);
	gpt_h->header_crc32 = 0;
	gpt_h->my_lba = 1;
	gpt_h->alternate_lba = params->size_bytes / params->logical_sector_size - 1;
	gpt_h->first_usable_lba = bh->start_of_partition;
	gpt_h->last_usable_lba = gpt_h->alternate_lba - 1 - 16384 / params->logical_sector_size;
	gpt_h->disk_guid = *disk_guid;
	gpt_h->partition_entry_lba = 2;
	gpt_h->partition_entry_count = 128;
	gpt_h->size_of_partition_entry = 128;
	gpt_h->partition_entry_array_crc32 = 0;
	return 0;
}

int build_secondary_gpt_header(struct gpt_header *gpt_h, struct gpt_header *primary_gpt_h, struct dev_params *params)
{
	memcpy(gpt_h->signature, "EFI PART", 8);
        gpt_h->revision = 0x00010000;
        gpt_h->header_size = sizeof(struct gpt_header);
        gpt_h->header_crc32 = 0;
        gpt_h->my_lba = primary_gpt_h->alternate_lba;
        gpt_h->alternate_lba = primary_gpt_h->my_lba;
        gpt_h->first_usable_lba = primary_gpt_h->first_usable_lba;
        gpt_h->last_usable_lba = primary_gpt_h->last_usable_lba;
        gpt_h->disk_guid = primary_gpt_h->disk_guid;
        gpt_h->partition_entry_lba = gpt_h->my_lba - 16384 / params->logical_sector_size;
        gpt_h->partition_entry_count = 128;
        gpt_h->size_of_partition_entry = 128;
        gpt_h->partition_entry_array_crc32 = 0;
return 0;
}

int build_bh(struct resfs_bh *bh, struct mkfs_args *args, struct uuid *uuid)
{
	memcpy(bh->bh_sig, BH_SIG, 16);
	bh->version_major = RESFS_VERSION_MAJOR;
	bh->version_minor = RESFS_VERSION_MINOR;
	bh->version_patch = RESFS_VERSION_PATCH;
	memcpy(bh->fs_uuid, uuid, sizeof(*uuid));
	if (args->label) {
		bh->label_len = (uint8_t)strlen(args->label);
		memcpy(bh->fs_label, args->label, bh->label_len);
	} 
	else {
		bh->label_len = 0;
	}
	bh->feature_flags |= FEAT_SNAPSHOTS;
	blake3_hash(bh, offsetof(struct resfs_bh, blake3_hash), bh->blake3_hash);
	return 0;
}

int build_eop(struct resfs_eop *eop, struct resfs_bh *bh)
{
	memcpy(eop->eop_sig, EOP_SIG, 8);
	eop->version_major = bh->version_major;
	eop->version_minor = bh->version_minor;
	eop->version_patch = bh->version_patch;
	eop->block_size = bh->block_size;
	eop->total_blocks = bh->total_blocks;
	memcpy(eop->fs_uuid, bh->fs_uuid, sizeof(bh->fs_uuid));
	eop->label_len = bh->label_len;
	memcpy(eop->fs_label, bh->fs_label, eop->label_len);
	eop->wia_start = bh->wia_start;
	eop->wia_size = bh->wia_size;
	eop->sr_start = bh->sr_start;
	eop->sr_size = bh->sr_size;
	eop->ir1_start = bh->ir1_start;
	eop->ir2_start = bh->ir2_start;
	eop->ir3_start = bh->ir3_start;
	eop->ir_size = bh->ir_size;
	eop->data1_start = bh->data1_start;
	eop->data2_start = bh->data2_start;
	eop->start_of_partition = bh->start_of_partition;
	eop->partition_size = bh->partition_size;
	eop->logical_sector_size = bh->logical_sector_size;
	blake3_hash(eop, offsetof(struct resfs_eop, blake3_hash), eop->blake3_hash);
	memcpy(eop->eop_tail, EOP_TAIL, 22);
	return 0;
}

int build_wia_h(struct resfs_wia_h *wia_h, struct resfs_bh *bh)
{
	memcpy(wia_h->wia_sig, WIA_SIG, 8);
	wia_h->capacity = (bh->wia_size - 1) * 4096 / 20;
	wia_h->data_offset = 2;
	return 0;
}

int build_sr_h(struct resfs_sr_h *sr_h)
{
	memcpy(sr_h->sr_sig, SR_SIG, 8);
	return 0;
}

int build_smi_h(struct resfs_smi_h *smi_h, struct resfs_bh *bh)
{
	memcpy(smi_h->smi_sig, SMI_SIG, 8);
	smi_h->used_blocks = 1;	
	uint64_t smi_body_blocks = (bh->ir_size - 2) * 2 / 5;
	smi_h->entry_count = smi_body_blocks * 4096 / sizeof(struct resfs_smi_entry);
	smi_h->file_counter = 1;
	smi_h->last_mount = now_ns();
	return 0;
}

int build_smi_root_entry(struct resfs_smi_entry *smi_entry, struct resfs_smi_h *smi_h, struct resfs_bh *bh)
{
	smi_entry->file_id = 1;
	uint64_t buffer_blocks = max(RESFS_MIN_BUFFER_BLOCKS, bh->total_blocks * 3 / 1000);
	smi_entry->seg0_lba = bh->data1_start + buffer_blocks;

	struct smi_body {
		struct resfs_smi_entry entry1;
		struct resfs_smi_entry entries[];
	} __packed;
	
	size_t size = sizeof(struct smi_body) + smi_h->entry_count * sizeof(struct resfs_smi_entry);
	struct smi_body *smi_body = calloc(1, size);
	smi_body->entry1 = *smi_entry;
	blake3_hash(smi_body, size, smi_h->blake3_hash);
	free(smi_body);

	return 0;
}

int build_dli_h(struct resfs_dli_h *dli_h, struct resfs_bh *bh, uint64_t ir_start)
{
	memcpy(dli_h->dli_sig, DLI_SIG, 8);
	uint64_t smi_body_blocks = (bh->ir_size - 2) * 2 / 5;
	uint64_t dli_body_blocks = (bh->ir_size - 2) - smi_body_blocks;
	dli_h->entry_count = dli_body_blocks * 4096 / 24;
	dli_h->data_offset = ir_start + smi_body_blocks + 2; 
	return 0;
}

int build_dli_root_entry(struct resfs_dli_entry *dli_entry, struct resfs_dli_h *dli_h)
{
	dli_entry->name_hash = blake3_hash_name("/");
	dli_entry->file_id = 1;
	dli_entry->parent_dir_id = 0;

	struct dli_body {
		struct resfs_dli_entry entry1;
		struct resfs_dli_entry entries[];
	} __packed;

	size_t size = sizeof(struct dli_body) + dli_h->entry_count * sizeof(struct resfs_dli_entry);
	struct dli_body *dli_body = calloc(1, size);
	dli_body->entry1 = *dli_entry;
	blake3_hash(dli_body, size, dli_h->blake3_hash);
	free(dli_body);

	return 0;
}

int build_root_seg(struct resfs_inline_seg0 *root_seg)
{
	memcpy(root_seg->seg_sig, SEG_SIG, 8);
	memcpy(root_seg->seg_end_sig, SEG_END_SIG, 8);
	root_seg->file_id = 1;
	root_seg->seg_index = 0;
	root_seg->flags = IS_FIRST_SEG | IS_INLINE | IS_DIRECTORY;
	root_seg->data_len = 10;
	root_seg->created_at = now_ns();
	memcpy(root_seg->filename, "/", 1);
	root_seg->filename_len = 1;
	root_seg->permissions = 0755;
	root_seg->modified_at = root_seg->created_at;
	memcpy(root_seg->data_region, ROOT_SIG, 10);
	blake3_hash(root_seg->data_region, sizeof(root_seg->data_region), root_seg->blake3_hash);
	root_seg->file_id_end = 1;
	root_seg->seg_index_end = 0;
	return 0;
}

int write_primary_gpt(struct dev_params *params, struct protective_mbr *mbr, struct gpt_header *primary_gpt_h, struct gpt_partition_entries *entries)
{
	write_lba(params->fd, 0, params, mbr, sizeof(struct protective_mbr));
	write_lba(params->fd, 1, params, primary_gpt_h, sizeof(struct gpt_header));
	write_lba(params->fd, 2, params, &entries->entry1, sizeof(struct gpt_partition_entry));
	uint64_t n = 16384 / params->logical_sector_size;
	for (uint64_t i = 1; i < n; i++) {
		write_lba(params->fd, 2 + i, params, entries, 0);
	}
	return 0;
}

int write_secondary_gpt(struct dev_params *params, struct gpt_header *secondary_gpt_h, struct gpt_partition_entries *entries)
{
	uint64_t n = 16384 / params->logical_sector_size;
	write_lba(params->fd, secondary_gpt_h->partition_entry_lba, params, &entries->entry1, sizeof(struct gpt_partition_entry));
	for (uint64_t i = 1; i < n; i++) {
		write_lba(params->fd, secondary_gpt_h->partition_entry_lba + i, params, entries, 0);
	}
	write_lba(params->fd, secondary_gpt_h->my_lba, params, secondary_gpt_h, sizeof(struct gpt_header));
	return 0;
}

/* GPT modification for disk partitions will be added later */
int alter_gpt() 
{
	return 0;
}

int main(int argc, char *argv[])
{
	crc32_init();
	struct mkfs_args args = {0};
	struct dev_params params = {0};
	struct protective_mbr mbr = {0};
	struct gpt_header primary_gpt_h = {0};
	struct gpt_partition_entry entry = {0};
	struct gpt_header secondary_gpt_h = {0};
	struct gpt_partition_entries entries = {0};
	struct resfs_bh bh = {0};
	struct resfs_eop eop = {0};
	struct resfs_wia_h wia_h = {0};
	struct resfs_sr_h sr_h = {0};
	struct resfs_smi_h smi_h1 = {0};
	struct resfs_dli_h dli_h1 = {0};
	struct resfs_smi_entry smi_entry1 = {0};
	struct resfs_dli_entry dli_entry1 = {0};
	struct resfs_smi_h smi_h2 = {0};
	struct resfs_dli_h dli_h2 = {0};
	struct resfs_smi_entry smi_entry2 = {0};
	struct resfs_dli_entry dli_entry2 = {0};
	struct resfs_smi_h smi_h3 = {0};
	struct resfs_dli_h dli_h3 = {0};
	struct resfs_smi_entry smi_entry3 = {0};
	struct resfs_dli_entry dli_entry3 = {0};
	struct resfs_inline_seg0 root_seg = {0};
	struct uuid disk_guid = {0};
	struct uuid part_uuid = {0};
	struct uuid part_guid = {0};
	part_guid.data1 = 0x46534552;
	part_guid.data2 = 0x4C49;
	part_guid.data3 = 0x5345;
	part_guid.data4[0] = 0x59;
	part_guid.data4[1] = 0x53;
	part_guid.data4[2] = 0x54;
	part_guid.data4[3] = 0x45;
	part_guid.data4[4] = 0x4D;
	part_guid.data4[5] = 0x2F;
	part_guid.data4[6] = 0x41;
	part_guid.data4[7] = 0x4B;

	int ret = parse_args(argc, argv, &args);
	if (ret == 1) {
		return 0;
	}

	else if (ret) {
		return 1;
	}

	if (check_disk(&args)) {
		return 1;
	}

	if (args.dev_type) {
		if (check_disk_type(&args, &params)) {
			return 1;
		}
	}

	if (confirm(&args, &params)) {
		return 1;
	}

	
	if (args.op_type) {
		if (get_params(&args, &params)) {
			return 1;
		}
	}

	else {
		if (create_image(&args, &params)) {
			return 1;
		}
	}

	if (args.dev_type) {
		if (params.is_disk_part) {
			if (!params.no_parent_dev_access) {
				printf("mkfs.resfs: error: disk partition formatting is not yet implemented\n");
				goto cleanup;
			}
		}
	}

	calc_layout(&params, &bh);

	if (generate_uuidv4(&part_uuid) || generate_uuidv4(&disk_guid)) {
		goto cleanup;
	}

	printf("Building GPT...\n");
	build_protective_mbr(&mbr, &params);
	build_gpt_partition_entry(&entry, &part_guid, &part_uuid, &bh);
	build_primary_gpt_header(&primary_gpt_h, &params, &bh, &disk_guid);
	build_secondary_gpt_header(&secondary_gpt_h, &primary_gpt_h, &params);
	entries.entry1 = entry;
	primary_gpt_h.partition_entry_array_crc32 = crc32(&entries, sizeof(struct gpt_partition_entries));
	primary_gpt_h.header_crc32 = crc32(&primary_gpt_h, sizeof(struct gpt_header));
	
	secondary_gpt_h.partition_entry_array_crc32 = crc32(&entries, sizeof(struct gpt_partition_entries));
	secondary_gpt_h.header_crc32 = crc32(&secondary_gpt_h, sizeof(struct gpt_header));

	printf("Building BH...\n");
	build_bh(&bh, &args, &part_uuid);

	printf("Building EOP segment...\n");
	build_eop(&eop, &bh);

	printf("Building WIA region...\n");
	build_wia_h(&wia_h, &bh);

	printf("Building SR...\n");
	build_sr_h(&sr_h);

	printf("Building IR1...\n");
	build_smi_h(&smi_h1, &bh);
	build_smi_root_entry(&smi_entry1, &smi_h1, &bh);
	build_dli_h(&dli_h1, &bh, bh.ir1_start);
	build_dli_root_entry(&dli_entry1, &dli_h1);

	printf("Building IR2...\n");
	build_smi_h(&smi_h2, &bh);
	build_smi_root_entry(&smi_entry2, &smi_h2, &bh);
	build_dli_h(&dli_h2, &bh, bh.ir2_start);
	build_dli_root_entry(&dli_entry2, &dli_h2);

	printf("Building IR3...\n");
	build_smi_h(&smi_h3, &bh);
	build_smi_root_entry(&smi_entry3, &smi_h3, &bh);
	build_dli_h(&dli_h3, &bh, bh.ir3_start);
	build_dli_root_entry(&dli_entry3, &dli_h3);

	printf("Building Root segment...\n\n");
	build_root_seg(&root_seg);

	printf("Writing Primary GPT...   ");
	write_primary_gpt(&params, &mbr, &primary_gpt_h, &entries);
	printf("Done.\n");

	printf("Writing Secondary GPT... ");
	write_secondary_gpt(&params, &secondary_gpt_h, &entries);
	printf("Done.\n");

	printf("Writing BH...            ");
	write_segment(&bh, &params, 0, &bh, sizeof(bh));
	printf("Done.\n");

	printf("Writing EOP segment...   ");
	write_segment(&bh, &params, bh.total_blocks - 1, &eop, sizeof(eop));
	printf("Done.\n");

	printf("Writing WIA region...    ");
	write_segment(&bh, &params, bh.wia_start, &wia_h, sizeof(wia_h)); 
	printf("Done.\n");

	printf("Writing SR...            ");
	write_segment(&bh, &params, bh.sr_start, &sr_h, sizeof(sr_h));
	printf("Done.\n");

	uint64_t smi_body_blocks = (bh.ir_size - 2) * 2 / 5;

	printf("Writing IR1...           ");
	write_segment(&bh, &params, bh.ir1_start, &smi_h1, sizeof(smi_h1));
	write_segment(&bh, &params, bh.ir1_start + 1, &smi_entry1, sizeof(smi_entry1));
	write_segment(&bh, &params, bh.ir1_start + 1 + smi_body_blocks, &dli_h1, sizeof(dli_h1));
	write_segment(&bh, &params, dli_h1.data_offset, &dli_entry1, sizeof(dli_entry1));
	printf("Done.\n");

	printf("Writing IR2...           ");
	write_segment(&bh, &params, bh.ir2_start, &smi_h2, sizeof(smi_h2));
	write_segment(&bh, &params, bh.ir2_start + 1, &smi_entry2, sizeof(smi_entry2));
	write_segment(&bh, &params, bh.ir2_start + 1 + smi_body_blocks, &dli_h2, sizeof(dli_h2));
	write_segment(&bh, &params, dli_h2.data_offset, &dli_entry2, sizeof(dli_entry2));
	printf("Done.\n");

	printf("Writing IR3...           ");
	write_segment(&bh, &params, bh.ir3_start, &smi_h3, sizeof(smi_h3));
	write_segment(&bh, &params, bh.ir3_start + 1, &smi_entry3, sizeof(smi_entry3));
	write_segment(&bh, &params, bh.ir3_start + 1 + smi_body_blocks, &dli_h3, sizeof(dli_h3));
	write_segment(&bh, &params, dli_h3.data_offset, &dli_entry3, sizeof(dli_entry3));
	printf("Done.\n");

	printf("Writing Root segment...  ");
	write_segment(&bh, &params, smi_entry1.seg0_lba, &root_seg, sizeof(root_seg));
	printf("Done.\n\n");

	printf("Filesystem created successfully!\n");
	printf("Partition UUID: %08x-%04x-%04x-%02x%02x%02x%02x%02x%02x%02x%02x\n", part_uuid.data1, part_uuid.data2, part_uuid.data3, part_uuid.data4[0], part_uuid.data4[1], part_uuid.data4[2], part_uuid.data4[3], part_uuid.data4[4], part_uuid.data4[5], part_uuid.data4[6], part_uuid.data4[7]);
	if (args.label) {
		printf("Partition label: %s\n", args.label);
	}
	printf("Root LBA: %li\n\n", smi_entry1.seg0_lba);

	if (params.fd >= 0) {
		close(params.fd);
	}
	if (params.pdev_fd >= 0) {
        	close(params.pdev_fd);
	}
	return 0;

cleanup:
	if (params.fd >= 0) {
		close(params.fd);
	}
	if (params.pdev_fd >= 0) {
        	close(params.pdev_fd);
	}
	return 1;
}