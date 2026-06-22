<img width="500" alt="ResFS logo" src="https://github.com/user-attachments/assets/c9eacbb5-728f-4cc1-b336-fd97bcf91ec1" />

Recovery-First File System (ResFS) is a file system in which every physically existing file is reconstructable from raw disk bytes even if all metadata, journals, and superblocks are completely destroyed. 

![Status](https://img.shields.io/badge/status-active-green)
![License](https://img.shields.io/badge/license-MIT-blue)

## What is ResFS?
ResFS is a file system which has no inode table or superblock. ResFS disk partitions have unique metadata and segment structure. ResFS is a segment-native FS, which means that every segment already contains its own metadata and every segment is a source of truth. All metadata structures are used only as an acceleration level. 

ResFS uses CoW (Copy-on-Write) algorithm for write operations and a log, called WIA (Write Intent Array) which helps the mount algorithm to find any aborted operations in crash recovery conditions. 

## ResFS Disk Partition Layout
```ResFS Partition
[Bootstrap Header (BH)]
[Write Intent Array (WIA)]
[Snapshot Region (SR)]
[Index Region 1 (IR1)]
[IR1 expansion buffer]

[DATA REGION 1]

[Index Region 2 (IR2)]
[IR2 expansion buffer]

[DATA REGION 2]

[IR3 expansion buffer]
[Index Region 3 (IR3)]
[End of Partition Segment (EOP)]
```
