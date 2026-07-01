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
	uint16_t boot_signature; /* 0x55AA */
} __packed;

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

struct gpt_partition_entry {
	struct uuid partition_type_guid; /* ResFS official GUID */
	struct uuid unique_partition_guid; /* UUIDv4 */
	uint64_t starting_lba;
	uint64_t ending_lba;
	uint64_t attributes; /* Not used in v0.99.1 */
	uint16_t partition_name[36]; /* UTF-16LE Partition name (default "ResFS Disk Partition") */
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
			printf("You are going to format a disk partition: %s!\n", args->device);
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
			printf("You are going to format a block device: %s!\n", args->device);
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
			printf("You are going to format an existing disk image: %s!\n", args->device);
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
			printf("You are going to create a disk image %s.\n", args->device);
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
		if (params->physical_sector_size != 4096) {
			fprintf(stderr, "mkfs.resfs: error: unsupported physical block size (must be 4KB)\n");
			close(fd);
			return ERR_INVALID;
		}
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
		fscanf(f, "%llu", &params->part_start_lba);
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
	bh->total_blocks = params->size_bytes / 4096;
	bh->logical_sector_size = params->logical_sector_size;
	bh->partition_size = bh->total_blocks * 4096 / bh->logical_sector_size;

	if (params->is_disk_part) {
		bh->start_of_partition = params->part_start_lba;
	} 
	else {
		uint64_t alignment_lba = 1048576 / params->logical_sector_size;
		bh->start_of_partition = align_up(34, alignment_lba);
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

int alter_gpt()
{
	return 0;
}

int write_gpt()
{
	return 0;
}

int write_bh()
{
	return 0;
}

int write_eop()
{
	return 0;
}

int write_ir1()
{
	return 0;
}

int write_ir2()
{
	return 0;
}

int write_ir3()
{
	return 0;
}

int write_root()
{
	return 0;
}

int main(int argc, char *argv[])
{
	crc32_init();
	struct mkfs_args args = {0};
	struct dev_params params = {0};
	struct resfs_bh bh = {0};
	struct resfs_eop eop = {0};

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
				if (get_pdev_params(&args, &params)) {
					return 1;
				}
			}
		}
	}

	/*tbd*/
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