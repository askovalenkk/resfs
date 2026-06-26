<img width="500" alt="ResFS logo" src="https://github.com/user-attachments/assets/d38c872a-aa85-49dd-93f1-c36d8085216e" />

Recovery-First File System (ResFS) is a file system in which every physically existing file is reconstructable from raw disk bytes even if all metadata, journals, and superblocks are completely destroyed. 

![Status](https://img.shields.io/badge/status-active-green)
![License](https://img.shields.io/badge/license-MIT-blue)

# What is ResFS?
ResFS is a file system which has no inode table or superblock. ResFS disk partitions have unique metadata and segment structure. ResFS is a segment-native FS, which means that every segment already contains its own metadata and every segment is a source of truth. All metadata structures are used only as an acceleration level. ResFS uses CoW (Copy-on-Write) algorithm for write operations, which guarantees that at least one correct file version still physically exists on the disk.

# Documentation
Official documentation of ResFS:

- [ResFS Specification](spec.md)

- [MIT License](LICENSE)

- ResFS Wiki (TBD)

# Project structure
```
resfs/
├── spec.md            
├── README.md
├── Makefile 
├── LICENSE 
├── .gitignore
├── libresfs/
│   ├── include/
│   │   └── resfs.h  
│   ├── src/
│   │   ├── bh.c
│   │   ├── cow.c
│   │   ├── dli.c
│   │   ├── eop.c
│   │   ├── mba.c
│   │   ├── mount.c
│   │   ├── recovery.c  
│   │   ├── segment.c 
│   │   ├── smi.c
│   │   ├── snap.c
│   │   └── wia.c
│   └── vendor/blake3/
├── tools/
│   ├── mkfs.c
│   ├── verify.c
│   ├── recover.c
│   ├── snap.c
│   ├── export.c
│   ├── import.c
│   └── visualize.c
└── tests/
    ├── mkfs.sh
    ├── verify.sh
    ├── recover.sh
    ├── search.sh
    ├── kill-9.sh
    ├── corrupt-gpt.sh
    └── corrupt-metadata.sh
```

# ResFS Disk Partition Layout

```
[Bootstrap Header (BH)]
[Write Intent Array (WIA)]
[Snapshot Region (SR)]
[Index Region 1 (IR1)]
[IR expansion buffer]

[ --- DATA REGION 1 --- ]

[Index Region 2 (IR2)]
[IR expansion buffer]

[ --- DATA REGION 2 --- ]

[IR expansion buffer]
[Index Region 3 (IR3)]
[End of Partition segment (EOP)]
```

### Bootstrap Header (BH)
Bootstrap Header is the first segment of the whole partition. It consists of partition identity (UUID, segment size, FS label, etc.), LBA boundaries of all data structures and the disk partition itself. BH segment starts with special signature which encodes as "RESFS PARTITION" in ASCII.

### End of Partition (EOP)
End of Partition segment is the last segment of the partition, which contains the same information as the Bootstrap Header. Tail signature at the end of the EOP segment encodes in ASCII as "END OF RESFS PARTITION". Readable signatures at the start and the end of every ResFS disk partition make it recognizable for disk parsers even if GPT is corrupted.

### Write Intent Array (WIA)
Write Intent Array is an operation log which allows the mount operation to detect segments committed before metadata update. ResFS is a Copy-on-Write file system, so old file version remains unchanged until the new one is fully written. In case of a crash during a CoW rewrite, the Index Region may not yet reflect the newly written segments. WIA allows the mount algorithm to locate and recover these committed but unindexed segments. If changes were not committed by the moment crash happened, IR will point to old segments.

### Snapshot Region (SR)
Snapshot Region is a fixed-size table located after WIA. Each SR entry holds a snapshot ID and a file ID of its corresponding snapshot file stored in the Data Region. Snapshot files contain a flat array of extents belonging to segments that were superseded by CoW rewrites after the snapshot was created.

During mount, the allocator reads SR to find all live snapshots, retrieves their file IDs, looks them up in SMI and marks their extents as occupied in the bitmap. This prevents the block allocator from overwriting segments that belong to live snapshots.

### Index Region (IR)
Index Region is a metadata acceleration structure which allows the FS to quickly locate file segments without scanning the entire disk. ResFS maintains three independent Index Region copies (IR1, IR2, IR3) distributed across the partition (IR1 is located at the start of partition, IR2 in the midpoint and IR3 in the end of partition). All three are kept in sync after every write operation using a sequential FIFO update order (IR1 → IR2 → IR3), which guarantees that at least one copy always contains consistent metadata even in case of a crash during IR update.

Each Index Region consists of two tables:

**Segment Map Index (SMI)** — maps every file ID to its SEG0 location. SMI is the primary structure used to locate file data during read operations.

**Directory Lookup Index (DLI)** — maps file and directory names to their file IDs. DLI enables fast path lookup without traversing directory segments on disk.

### IR Expansion
Each IR has initial size defined by layout calculations at mkfs. Although Index Regions usually remain the initial size, in case of IR overflow they can be expanded into expansion buffers. Block allocator doesn't write any data into expansion buffers unless necessary. Any data placed there will be rewritten using CoW.

# Building

### Dependencies
- gcc
- make
- BLAKE3 (vendored, no installation required)

### Build
ResFS is built as a static library `libresfs.a` which contains all core filesystem logic. Each tool in `tools/` is compiled separately and linked against it.

Running `make` compiles everything at once:
```bash
make
```

This produces:
- `libresfs.a` — core library
- `tools/mkfs` — formats a disk image or partition as ResFS
- `tools/verify` — verifies partition integrity
- `tools/recover` — manual recovery tool
- `tools/snap` — snapshot management
- `tools/export` — extract raw file or recovery container from ResFS
- `tools/import` — import from ext4/NTFS/exFAT/APFS to ResFS
- `tools/visualize` — ASCII visualization of segment and free space layout

### Clean
Removes all compiled objects, `libresfs.a` and tool binaries:
```bash
make clean
```

# Testing

After cloning it with the command:

```bash
git clone https://github.com/askovalenkk/resfs
```

You can run the bash scripts from `tests/`:
- `mkfs.sh` — makes a disk image and formats it with a ResFS partition
- `verify.sh` — check disk for any corruptions
- `recover.sh` — finds and recovers all existing files on the disk partition
- `search.sh` — finds LBA boundaries of the ResFS disk partition without GPT
- `kill-9.sh` — runs *kill -9* while writing files on the disk partition
- `corrupt-gpt.sh` — target *dd if=/dev/zero...* for both GPT copies of the ResFS disk image
- `corrupt-metadata.sh` — target *dd if=/dev/zero...* for all disk metadata

# License
ResFS is released under the MIT License. This license covers all original source code in this repository. The vendored BLAKE3 implementation (`libresfs/vendor/blake3`) is released under the CC0 license.
