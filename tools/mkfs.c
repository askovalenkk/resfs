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
	int force; 
	int dev_type; /* 0 = disk image; 1 = block device */
	int op_type; /* 0 = create new disk image; 1 = format*/
	double sr_percent;
};

static uint32_t crc32_table[256];

static void crc32_init(void) {
	for (uint32_t i = 0; i < 256; i++) {
        	uint32_t c = i;
        	for (int j = 0; j < 8; j++)
            		c = (c >> 1) ^ (c & 1 ? 0xEDB88320 : 0);
        	crc32_table[i] = c;
	}
}

static uint32_t crc32(const void *buf, size_t len) {
	const uint8_t *p = buf;
	uint32_t c = 0xFFFFFFFF;
    	while (len--)
		c = (c >> 8) ^ crc32_table[(c ^ *p++) & 0xFF];
    	return c ^ 0xFFFFFFFF;
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
			return 0;
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
			perror("stat");
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

int confirm(struct mkfs_args *args) {
	if (args->force) {
		return 0;
	}
	else if (args->dev_type == 1) {
		printf("You are going to format an existing block device!\n");
		printf("Continue? [Y/n]: ");
		char ans = getchar();
		if (ans == 'Y' || ans == 'y') {
			printf("\n");
			return 0;
		}
		else {
			printf("\n");
			return 1;
		}
	}
	else if (args->dev_type == 0) {
		if (args->op_type) {
			printf("You are going to format an existing disk image!\n");
			printf("Continue? [Y/n]: ");
			char ans = getchar();
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
			printf("You are going to create a disk image.\n");
			printf("Continue? [Y/n]: ");
			char ans = getchar();
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



int create_image()
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

int write_irs()
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
	if (parse_args(argc, argv, &args)) {
		return 1;
	}
	if (check_disk(&args)) {
		return 1;
	}
	if (confirm(&args)) {
		return 1;
	}
}