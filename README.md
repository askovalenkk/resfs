<img width="500" alt="ResFS logo" src="https://github.com/user-attachments/assets/c9eacbb5-728f-4cc1-b336-fd97bcf91ec1" />

Recovery-First File System (ResFS) is a file system in which every physically existing file is reconstructable from raw disk bytes even if all metadata, journals, and superblocks are completely destroyed. 

![Status](https://img.shields.io/badge/status-active-green)
![License](https://img.shields.io/badge/license-MIT-blue)

## What is ResFS?
ResFS is a file system which has no inode table or superblock. ResFS disk partitions have unique metadata and segment structure. ResFS is a segment-native FS, which means that every segment already contains its own metadata and every segment is a source of truth. All metadata structures are used only as an acceleration level. 

ResFS uses CoW (Copy-on-Write) algorithm for write operations and a log, called WIA (Write Intent Array) which helps the mount algorithm to find any aborted operations in crash recovery conditions. 

## ResFS Disk Partition Layout

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

### Index Region (IR)
Index Region is a metadata acceleration structure which allows the FS to quickly locate file segments without scanning the entire disk. ResFS maintains three independent Index Region copies (IR1, IR2, IR3) distributed across the partition (IR1 is located at the start of partition, IR2 in the midpoint and IR3 in the end of partition). All three are kept in sync after every write operation using a sequential FIFO update order (IR1 → IR2 → IR3), which guarantees that at least one copy always contains consistent metadata even in case of a crash during IR update.

Each Index Region consists of two tables:

**Segment Map Index (SMI)** — maps every file ID to its extents on disk. SMI is the primary structure used to locate file data during read operations.

**Directory Lookup Index (DLI)** — maps file and directory names to their file IDs. DLI enables fast path lookup without traversing directory segments on disk.

### IR Expansion
Each IR has fixed size defined by initial layout calculations at mkfs. Although Index Regions usually remain the initial size, in case of IR overflow they can be expanded into expansion buffers. Block allocator doesn't write any data into expansion buffers unless necessary. Any data placed there will be rewritten using CoW.
