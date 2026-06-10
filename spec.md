<img width="200" alt="image" src="https://github.com/user-attachments/assets/6be003cb-7e79-4787-b943-78ba63853499" /> 
# ResFS Specification v1.5
> Recovery-First Filesystem — every physically intact segment is recoverable,
> deterministically, without heuristics, even if all metadata is destroyed.

## What ResFS Is (and Is Not)

**ResFS is:**
- A forensic-recovery-first filesystem
- A segment-native filesystem where metadata destruction is irrelevant by design
- A universal open standard for embedded, desktop, server, and OS use
- A filesystem where every physically intact segment is recoverable

---

## Core Philosophy

**ResFS is not an inode filesystem. ResFS is a segment-native filesystem.**

Segments are authoritative. All indexes, maps, and caches are
disposable acceleration structures.

If every metadata structure is destroyed, the filesystem remains
reconstructable from segment data alone.

### Sources of Truth

```
Truth:
    Data Segments       ← only authoritative source
    Directory Segments  ← authoritative for namespace

Mount Acceleration:
    SMI (Segment Map Index)     ← physical layer cache
    DLI (Directory List Index)  ← namespace layer cache
    WIR (Write Intent Record)   ← crash recovery accelerator

Catastrophic Recovery:
    Full Disk Scan      ← always works if segments are intact
```

**Nothing except segments is required for reconstruction.**

There is no journal. ResFS is a pure CoW filesystem. Crash safety comes
from Copy-on-Write atomicity at the segment level. IS_COMMITTED is the
atomic commit gate.

### Recovery Guarantee

Every physically intact segment is recoverable.

If a segment exists on disk and has not been physically overwritten
or destroyed by media damage — it will be found, verified by BLAKE3,
and reconstructed. No heuristics. No guessing. Deterministic.

ResFS does not guarantee recovery of all files after physical media damage.
ResFS guarantees recovery of every physically intact segment.

### Conflict Resolution (immutable rules)

**Segments always win. No exceptions.**

| Conflict | Resolution |
|----------|-----------|
| SMI says file has extents [A,B,C], segments show [A,C] | Segments win. SMI rebuilt. |
| DLI says file exists, no segments found for file_id | File does not exist. DLI entry purged. |
| Two segments with identical file_id + seg_index, both valid BLAKE3 | Take segment at lower LBA (deterministic). |
| Segment missing in middle of chain (gap) | File partially recovered, gap filled with zeros, recovery_info written. |

---

## Official GUID

ResFS partition type GUID for GPT:

```
52455346-494C-4553-5953-54454D2F414B
```

Encodes "RESFILESYSTEM/AK" in ASCII — filesystem name and author initials.
Every ResFS partition on every disk carries this signature.
This is part of the standard — permanent and immutable.

---

## Why These Design Choices

### Why BLAKE3?

| Property       | CRC32 | SHA-256 | BLAKE3  |
|----------------|-------|---------|---------| 
| Speed          | Fast  | Medium  | Fastest |
| Output size    | 4B    | 32B     | 32B     |
| Cryptographic  | No    | Yes     | Yes     |
| Parallelizable | No    | No      | Yes     |
| Safe for dedup | No    | Yes     | Yes     |

CRC32 can collide — any integrity check based on CRC32 can silently
pass corrupted data. BLAKE3 is collision-resistant, faster than SHA-256,
and parallelizes naturally across segment data.

### Why CoW instead of journaling?

Journaling solves crash consistency by logging intent before acting.
CoW solves it differently: the old segment is never overwritten until
the new segment is fully committed. If power is lost mid-write, the old
version survives intact. IS_COMMITTED is the atomic commit gate.

```
Old segment (IS_COMMITTED) → still valid, still readable
New segment being written  → IS_COMMITTED not yet set → invisible to FS
Power loss here            → old version survives, new is discarded on next mount
New segment gets IS_COMMITTED → new version visible, old is superseded
```

No journal needed. No replay. Recovery is always deterministic from segments.

### Why WIR instead of a journal?

A journal logs intent and replays operations on recovery. WIR does neither.
WIR only records where new segments were written — so that on dirty mount,
the scanner knows exactly which blocks to check for IS_COMMITTED, instead
of scanning the entire Data Region.

WIR is a recovery accelerator, not a journal. No replay. No redo.
If WIR itself is corrupt or missing, dirty mount falls back to full
Data Region scan. The result is always identical — WIR only affects speed.

### Why extents in SMI?

Instead of storing each segment offset individually:
```
file_id → [100, 101, 102, 103, 104]   ← 5 entries
```
Extents collapse contiguous runs:
```
file_id → { extent(start=100, len=5) }  ← 1 entry
```
For a 1GB file with no fragmentation: 1 extent vs 262144 entries.
SMI size drops by orders of magnitude for large sequential files.

### Why file_id is a plain u64 counter?

Simple, small, and recoverable. At 1,000,000 new files per second a u64
counter overflows in ~585,000 years — more than sufficient for any use case.

On recovery without valid IR: scan all segments, find the maximum file_id
value, resume from max+1. Trivially O(n), no set membership problem.

Deleted file_ids are never recycled.

Previous designs used counter+CSPRNG (128-bit). The random half provided
no meaningful benefit: the counter half already guarantees uniqueness, and
BLAKE3-verified segments make file_id guessing attacks irrelevant. The
random half wasted 8 bytes per segment header and footer — on a 1TB disk
this amounts to ~4GB of meaningless data.

### Why 4KB segments?

Universal hardware alignment — HDD, SSD, NVMe, NAND flash.
SSD internal pages are 4KB so writes are naturally atomic at this size.

### Why Bootstrap Header instead of superblock?

A traditional superblock accumulates every kind of state: identity,
counters, layout pointers, mount history. It becomes a critical single
point of failure and a philosophical contradiction in a recovery-first
filesystem.

ResFS separates concerns cleanly:
- **BH** carries only immutable identity + layout (written once at mkfs)
- **SMI header** carries allocation state (file_counter, used_blocks)
- **Segments** carry everything else

If BH is destroyed, EOP provides partition boundaries and IR hints.
If all IRs are destroyed, segments reconstruct everything.
Nothing is irreplaceable.

---

## Disk Layout

```
[ResFS Partition]
  Block 0               Bootstrap Header (BH)
  Block 1..wir_end      WIR Region — fixed size, allocated at mkfs
  Block wir_end+1..     Index Region 1 (IR1) — fixed size, allocated at mkfs
  Block ir1_end+1..     Snapshot Region — fixed size, allocated at mkfs
  Block snap_end+1..    Data Region 1 (segments)
  [partition midpoint]  Index Region 2 (IR2) — fixed size, allocated at mkfs
  [continued]           Data Region 2 (segments, continued)
  [near end]            Index Region 3 (IR3) — fixed size, allocated at mkfs
  [last block]          End Of Partition marker (EOP)
```

**WIR Region is fixed-size**, allocated at mkfs immediately after BH.
Its location is always known: block 1. No pointer needed.

**Index Regions are fixed-size**, allocated at mkfs. They never grow into
adjacent regions. Unused space within an IR region is zeroed. As the SMI
and DLI grow, they expand into the pre-allocated zeroed space.

IR3 ends at `last_lba - 1` at maximum. EOP always occupies `last_lba`.

Three Index Regions distributed across the partition:
- **IR1**: immediately after Snapshot Region
- **IR2**: at partition midpoint
- **IR3**: near partition end (right before EOP)

Each IR = 1 SMI + 1 DLI. Three IRs = three independent copies of SMI+DLI,
physically separated. BH carries the exact LBA of all three IRs.

---

## Bootstrap Header (BH)

The BH carries partition identity, structural layout pointers,
and the FS_DIRTY flag. It is not a superblock — it holds no file
metadata, no counters, and no mount history.

FS_DIRTY is set before every write operation and cleared after
IR update. All other BH fields change only on structural events
(IR expansion). At mkfs, all fields except FS_DIRTY are written
once and remain stable for the lifetime of the partition.

```
Offset  Size    Field           Description
------  ----    -----           -----------
0       16      BH_SIG          Magic: "RESFS PARTITION "
16      4       version         u32, format version (1)
20      4       block_size      u32, always 4096
24      16      fs_uuid         UUID of this filesystem instance
40      1       label_len       u8, length of fs_label
41      255     fs_label        UTF-8 filesystem label, null-padded
296     4       feature_flags   u32, see Feature Flags section
300     8       wir_start       u64, LBA of WIR Region (always 1)
308     8       wir_size        u64, size of WIR Region in blocks
316     8       ir1_start       u64, LBA of IR1 (fixed at mkfs, never changes)
324     8       ir2_start       u64, LBA of IR2 (fixed at mkfs, never changes)
332     8       ir3_start       u64, LBA of IR3 (moves left on expansion)
340     8       ir_size         u64, size of each IR in blocks (all equal, grows on expansion)
348     8       snap_start      u64, LBA of Snapshot Region (= ir1_start + ir_size)
356     8       snap_size       u64, size of Snapshot Region in blocks
364     8       data1_start     u64, LBA of Data Region 1 (= snap_start + snap_size)
372     8       data2_start     u64, LBA of Data Region 2 (= ir2_start + ir_size)
380     8       total_blocks    u64, total blocks in partition
388     32      blake3_hash     BLAKE3 of bytes [0..387]
420     3676    reserved        Must be zero (pad to 4096 bytes)
```

### Feature Flags

```
Bit     Meaning
---     -------
0       FEAT_ENCRYPTION     per-segment AES-256-GCM supported
1       FEAT_COMPRESSION    ZSTD compression supported
2       FEAT_SNAPSHOTS      CoW snapshots supported
3       FEAT_SPARSE         sparse files supported
4       FEAT_ACL            ACL entries supported
5       FEAT_XATTR          extended attributes supported
6       Reserved, must be 0
7       FS_DIRTY            set before any write, cleared after IR update
                            if set on mount → WIR-guided dirty mount procedure
8-31    Reserved, must be 0
```

### Initial Layout Calculation (at mkfs)

```
wir_size  = max(MIN_WIR_BLOCKS, total_blocks / 1000)
MIN_WIR_BLOCKS = 8 (32KB)

ir_size   = max(MIN_IR_BLOCKS, total_blocks * 3 / 1000)
MIN_IR_BLOCKS = 768 (3MB)

wir_start = 1
ir1_start = wir_start + wir_size
snap_start = ir1_start + ir_size
data1_start = snap_start + snap_size
ir2_start = total_blocks / 2
ir3_start = total_blocks - ir_size - 1 - buffer_blocks
EOP: last_lba = total_blocks - 1

buffer_blocks = max(1280, total_blocks * 5 / 1000)  (~0.5%, min 5MB)
```

---

## End Of Partition (EOP)

EOP is a physical boundary marker. It occupies the last block of the
partition (`last_lba`). It is not part of any IR. It is not a cache.
It is not a source of truth about files.

EOP is used only during recovery when GPT is destroyed and partition
boundaries are unknown. It is not required for normal operation.

```
Offset  Size    Field           Description
------  ----    -----           -----------
0       8       EOP_SIG         Magic: "ResFSEOP"
8       4       version         u32 = 1
12      4       block_size      u32 = 4096
16      16      fs_uuid         UUID of this partition (matches BH)
32      8       part_start_lba  u64, LBA of partition start
40      8       part_end_lba    u64, LBA of partition end (this block)
48      8       wir_start       u64, LBA of WIR Region (hint only)
56      8       wir_size        u64, size of WIR Region in blocks (hint only)
64      8       ir1_lba         u64, LBA of IR1 (hint only)
72      8       ir2_lba         u64, LBA of IR2 (hint only)
80      8       ir3_lba         u64, LBA of IR3 (hint only)
88      8       data1_start     u64, LBA of Data Region 1 (hint only)
96      8       data2_start     u64, LBA of Data Region 2 (hint only)
104     32      blake3_hash     BLAKE3 of bytes [0..103]
136     3928    reserved        Must be 0x00
4064    32      EOP_TAIL        \x00\x00..."END OF RESFS PARTITION"
```

EOP_TAIL layout (32 bytes):
```
[10 bytes: 0x00] ["END OF RESFS PARTITION"]
```

On hex dump the last block ends visually as:
```
...00 00 45 4E 44 20 4F 46 ..END OF
   20 52 45 53 46 53 20 50 RESFS P
   41 52 54 49 54 49 4F 4E ARTITION
```

**EOP is 4096 bytes (one block), fixed.**

### EOP Usage During Recovery

```
1. GPT intact → partition boundaries known → EOP not needed

2. GPT destroyed → scan last N blocks of each device:
   if block[0..7] == "ResFSEOP":
     verify BLAKE3([0..103])
     if valid and fs_uuid matches → EOP found

3. EOP found:
   part_start_lba → partition start
   part_end_lba   → partition end
   wir_start/wir_size → hints to locate WIR
   ir1/ir2/ir3_lba → hints to locate IR copies
   data1_start / data2_start → hints for Data Regions

4. Attempt to load BH from part_start_lba:
   if BH valid → use all locations from BH (authoritative)
   if BH invalid → use hints from EOP

5. Attempt to load IR copies:
   if valid IR found → mount with SMI+DLI
   if no valid IR → full segment scan from data1_start to part_end_lba

6. Full segment scan → rebuild SMI+DLI from segments
   segments are the only truth
```

### EOP Conflict Resolution

```
EOP absent or not written:
  → system operates normally, EOP is optional
  → recovery without GPT falls back to brute-force IR scan

EOP BLAKE3 invalid:
  → EOP ignored entirely
  → scanner searches for IR by "ResFSSMI" magic brute-force

EOP fs_uuid does not match BH:
  → EOP ignored (foreign partition or corruption)
  → BH always wins on uuid conflict

EOP hints conflict with BH:
  → BH wins (BH is authoritative, EOP hints are advisory)
  → EOP hints used only when BH is unavailable
```

### EOP Invariants

| Invariant | Status |
|-----------|--------|
| segments = only truth about files | ✅ EOP contains no file data |
| IR = cache only | ✅ EOP is not an IR, contains no SMI/DLI |
| no metadata outranks segments | ✅ EOP contains only physical boundaries |
| EOP is not required | ✅ FS operates fully without EOP |

---

## WIR — Write Intent Record

The WIR Region is a fixed-size pool of write intent entries, allocated
at mkfs. It lives at block 1 — immediately after BH — and is always
findable without any pointer.

WIR is a crash recovery accelerator. It records where new segments were
written before IR is updated. On dirty mount, the scanner reads WIR and
checks only those specific blocks for IS_COMMITTED, instead of scanning
the entire Data Region.

**WIR is not a journal.** No operations are replayed. No redo log.
If IS_COMMITTED is not set on the new SEG 0, the write is simply
discarded — the old version survives in SMI. WIR only tells the
scanner where to look.

If WIR itself is corrupt or absent on dirty mount, the fallback is
a full Data Region scan. The result is always identical — WIR only
affects speed, never correctness.

### WIR Header (4096 bytes, fixed)

```
Offset  Size    Field           Description
------  ----    -----           -----------
0       8       WIR_SIG         Magic: "ResFSWIR"
8       4       version         u32
12      4       reserved        u32, must be 0
16      8       generation      u64, incremented on each WIR write
24      8       entry_count     u64, number of active entries
32      8       capacity        u64, maximum entries (computed from wir_size)
40      8       data_offset     u64, byte offset to offset table (relative to WIR start)
48      32      blake3_hash     BLAKE3 of entire WIR body (header + table + entries)
80      4016    reserved        pad to 4096 bytes
```

### WIR Offset Table

Immediately after WIR Header:

```
entry_count × 8 bytes
each entry: u64 byte offset of a WIR Entry (relative to WIR Region start)
```

### WIR Entry (variable length)

```
Offset  Size            Field       Description
------  ----            -----       -----------
0       8               file_id     u64
8       4               operation   u32, see WIR Operations
12      4               ext_count   u32, number of new extents
16      ext_count × 24  extents     new extents (same format as SMI Extent)
```

### WIR Operations

```
WRITE   = 1    regular CoW write or append
DEFRAG  = 2    defragmenter CoW relocation
EXPAND  = 3    IR expansion CoW relocation
```

### WIR Write Path

WIR entry is written before any CoW operation begins:

```
1. Write WIR entry (file_id, operation, new extents)
2. Verify BLAKE3 of WIR
3. Proceed with CoW operation
```

Crash during step 1: WIR BLAKE3 invalid → WIR entry ignored on dirty mount.
No segments have been written yet → filesystem is consistent.

Crash after step 1: WIR entry is valid → dirty mount reads it → checks
only the listed blocks for IS_COMMITTED.

### WIR on Dirty Mount

```
1. Read WIR Header — verify BLAKE3
   if invalid → fallback to full Data Region scan
2. For each WIR entry:
   a. Read listed extent blocks
   b. Check SEG 0 for IS_COMMITTED
   c. IS_COMMITTED=1 → write was committed, IR not yet updated
      → add extents to SMI
   d. IS_COMMITTED=0 → write was aborted
      → mark blocks free (allocator reclaims them)
      → if operation=DEFRAG → file remains in defrag queue
3. Clear WIR (zero entry_count, recompute BLAKE3)
4. Update all three IR copies
5. Clear FS_DIRTY in BH
```

### WIR Capacity

```
capacity = (wir_size * 4096 - 4096) / avg_entry_size
```

On a 1TB disk (wir_size ≈ 256 blocks): capacity >> 1000 entries.
On a 64MB embedded disk (wir_size = 8 blocks, 32KB): capacity ≈ 100 entries.

WIR capacity scales automatically with disk size. No configuration needed.

---

## Segment Layout (4096 bytes total)

```
Offset  Size    Field           Description
------  ----    -----           -----------

=== HEADER (64 bytes) ===

0       8       SEG_SIG         Magic: "ResFSSEG"
8       8       file_id         u64, monotonic counter
16      4       seg_index       u32, position in file (0-based)
20      4       flags           u32, see Flags section
24      4       data_len        u32, actual bytes in data region (≤ 4008)
28      4       reserved        Must be 0x00000000
32      8       file_size       u64, total file size in bytes
                                (0 for IS_DIRECTORY)
40      32      blake3_hash     BLAKE3-256 of data region [64..4071]

=== DATA REGION (4008 bytes) ===

64      4008    data            Raw file data (or file header for SEG 0)

=== FOOTER (24 bytes) ===

4072    8       SEG_END_SIG     Magic: "ResFSEND"
4080    8       file_id         u64, repeated for footer validation
4088    4       seg_index       u32, repeated for footer validation
4092    4       reserved        Must be 0x00000000
```

**Overhead: 88 bytes / 4096 = 2.15%**
**Usable data: 4008 bytes per segment**

---

## Flags Field

```
Bit     Meaning
---     -------
0       IS_FIRST_SEG        seg_index == 0, data starts with file header
1       IS_LAST_SEG         final segment, data_len may be ≤ 4008
2       IS_COMPRESSED       data is ZSTD compressed
3       IS_ENCRYPTED        data is AES-256-GCM encrypted
4       HAS_FILENAME        first segment has filename header
5       IS_DELETED          soft delete marker (on SEG 0 only)
6       IS_COMMITTED        CoW commit gate — segment is visible to the FS
7       IS_DIRECTORY        this file is a directory
8       IS_SYMLINK          this file is a symbolic link
9       IS_DEVICE_FILE      this file is a device file
10      IS_SPARSE_SEG       placeholder for sparse hole (data_len = 0)
11      IS_XATTR_SEG        this segment contains extended attributes
12      IS_SNAPSHOT_SEG     this segment is retained for a snapshot
13      IS_ACL_SEG          this segment contains ACL entries
14-31   Reserved, must be 0
```

**IS_COMMITTED is the CoW commit gate. It lives only on SEG 0.**

IS_COMMITTED means: "this file has been written but IR has not yet been
updated." It is a temporary flag, not a permanent attribute.

Normal state of all files: IS_COMMITTED is NOT set.
IS_COMMITTED is set only during the window between segment write and IR update.

**IS_LAST_SEG and data_len = 4008:**
If a file's size is an exact multiple of 4008, the last segment has
`IS_LAST_SEG` set and `data_len = 4008`. This is valid and unambiguous.

**IS_SPARSE_SEG:**
Sparse segments physically exist on disk so the scanner finds them,
but carry no data (`data_len = 0`). They mark holes in sparse files.

---

## First Segment File Header (SEG 0)

When `IS_FIRST_SEG | HAS_FILENAME` is set, data region begins with:

```
Offset  Size    Field
------  ----    -----
64      1       filename_len    u8, length of filename in bytes
65      255     filename        UTF-8, null-padded
320     4       reserved        Must be 0x00000000
324     4       permissions     Unix rwxrwxrwx + setuid/setgid/sticky (12 bits)
328     8       created_at      u64, Unix timestamp nanoseconds
336     8       modified_at     u64, Unix timestamp nanoseconds
344     8       owner_uid       u64
352     8       owner_gid       u64
360     8       hardlink_id     u64, shared file_id if hardlink, else 0
368     ...     actual data     File data starts here
```

**SEG 0 header overhead: ~304 bytes**
(only one SEG 0 per file regardless of file size)

---

## File ID

```c
typedef uint64_t file_id_t;
```

File IDs are plain monotonic u64 counters. Simple, small, recoverable.

- At 1,000,000 new files/second: overflow in ~585,000 years
- On recovery without valid IR: scan all segments, take max(file_id) + 1
- Deleted file_ids are never recycled

The current counter value (`file_counter`) lives in the SMI header.
In memory: incremented atomically on every CREATE. Implementation must
guarantee atomicity — the mechanism is platform-defined.
On disk: written to IR on checkpoint and unmount.
On crash: recovered from max(file_id) across all committed segments.

### Reserved file_ids

```
0   NULL — reserved, means "no file" / "no parent"
            used as parent_dir_id of root directory
            used as hardlink_id when file is not a hard link
1   Root directory (always file_id = 1)
```

---

## Root Directory

```
file_id        = 1
parent_dir_id  = 0  (sentinel, no parent)
flags          = IS_FIRST_SEG | HAS_FILENAME | IS_DIRECTORY | IS_COMMITTED
filename       = "/"
data[0..10]    = "RESFS ROOT "   (signature at start of data)
```

The root directory signature `"RESFS ROOT "` precedes directory entries
in the data region of SEG 0. On recovery, the scanner identifies the root
by `file_id = 1` — the signature is a human-readable confirmation.

---

## Sparse Files

A sparse segment is a placeholder for a hole — a zero region with no
physical data. `IS_SPARSE_SEG` set → `data_len = 0`.

The scanner finds sparse segments and maps their `seg_index` positions
as zero regions. Physical space usage is proportional to actual content.

Example: a 50GB VM disk image with mostly empty space uses only segments
with real data plus sparse placeholders.

---

## Directories

Directories are regular files with `IS_DIRECTORY` flag.
No special treatment — same segments, same BLAKE3, same recovery.
`file_size = 0` always for directories (size is meaningless).

### Directory Entry Format

Variable-length entries packed sequentially in the data region:

```
[1 byte: name_len][name_len bytes: name][8 bytes: file_id][4 bytes: flags]
```

Minimum entry (1-char name): 14 bytes.
Average entry (~25-char name): ~38 bytes → ~105 entries per segment.

Directory entry flags:

```
0x01    ENTRY_DIR       this entry points to a directory
0x02    ENTRY_SYMLINK   this entry points to a symlink
0x04    ENTRY_DELETED   soft-deleted entry (tombstone)
```

Special entries:

```
file_id = 0   name = "."   → self reference
file_id = parent_dir_id  name = ".."  → parent directory
```

---

## Symbolic Links

Symlinks are regular files with `IS_SYMLINK` flag.
Data region of SEG 0 (after the file header) contains the target path
as a UTF-8 string. If the target is deleted, the symlink is broken
(standard Unix behavior).

---

## Hard Links

Hard links share data segments but have independent first segments.

Each hard link has its own unique `file_id` and its own SEG 0 containing
its own name and metadata. All hard links to the same data share a common
`hardlink_id` (the original file's `file_id`).

Data segments (SEG 1, SEG 2, ...) exist once under the original file_id.

```
link1: file_id=3, hardlink_id=5, name="foto.jpg"   ← own SEG 0
link2: file_id=4, hardlink_id=5, name="backup.jpg" ← own SEG 0
data:  file_id=5, SEG 1, SEG 2, ...                ← shared data
```

Reference count is implicit: count of live (IS_COMMITTED, not IS_DELETED)
SEG 0 segments sharing the same `hardlink_id`.
On recovery: scan → count → determine liveness. Fully deterministic.

---

## Device Files

Device files use `IS_DEVICE_FILE` flag. SEG 0 data region contains
(after the standard file header):

```
Offset  Size    Field
------  ----    -----
368     4       major_number    u32
372     4       minor_number    u32
376     8       driver_id       u64
384     4       device_flags    u32
388     4       device_type     u32
```

Device types:

```
DEVICE_TYPE_BLOCK   = 0x01
DEVICE_TYPE_CHAR    = 0x02
DEVICE_TYPE_VIRTUAL = 0x03
```

---

## Extended Attributes (xattr)

xattr segments use `IS_XATTR_SEG` flag with the same `file_id`.
Recovered automatically alongside file data.

Data region contains variable-length key-value pairs:

```
[1 byte key_len][key_len bytes key][4 bytes value_len][value_len bytes value]
```

---

## Access Control Lists (ACL)

ACL segments use `IS_ACL_SEG` flag with the same `file_id`.

Data region contains fixed-length ACL entries:

```
[8 bytes subject_id][4 bytes permissions][4 bytes type]
```

ACL types: `ACL_USER = 0x01`, `ACL_GROUP = 0x02`,
`ACL_OTHER = 0x03`, `ACL_MASK = 0x04`

---

## Snapshots (Copy-on-Write)

When a segment is modified under an active snapshot, the old segment
is retained for the snapshot before the new version becomes live.

Snapshot metadata lives in the Snapshot Region (fixed, after IR1):

```
Offset  Size    Field
------  ----    -----
0       8       SNAP_SIG        "ResFSSNP"
8       8       snapshot_id     u64
16      8       created_at      u64, Unix timestamp nanoseconds
24      255     label           UTF-8 human-readable name
279     8       root_dir_id     file_id of root directory at snapshot time
287     1       is_deleted      0 = live, 1 = pending GC
288     32      blake3_hash     BLAKE3 of bytes [0..287]
```

If the Snapshot Region is full, `RESFS_ERR_SNAP_FULL` is returned.
The user must explicitly delete an existing snapshot via `resfs-snap`
before a new one can be created. Snapshots are never deleted automatically.

### Write Path Under Active Snapshot

When a file is modified and one or more snapshots are active:

```
1. Set FS_DIRTY in BH
2. Write WIR entry
3. Write new segments to free blocks
4. fsync + verify BLAKE3
5. Set IS_SNAPSHOT_SEG on old segments   ← retain for snapshot, before GC can touch them
6. Set IS_COMMITTED on new SEG 0         ← atomic commit point
7. Update IR copies
8. Clear IS_COMMITTED from new SEG 0
9. Clear WIR entry
10. Clear FS_DIRTY in BH
```

Step 5 must happen before step 6 — old segments are marked for
snapshot retention before the new version becomes live.
GC never touches IS_SNAPSHOT_SEG segments.

### Snapshot GC

**Phase 1 — Mark (instant):** Set `is_deleted = 1`. Snapshot immediately
invisible to the filesystem. No segments touched.

**Phase 2 — Sweep (lazy, background):** `resfs-gc` loads the Snapshot
Region at startup and builds the set of deleted snapshot_ids. It then
scans all `IS_SNAPSHOT_SEG` segments, checks `snapshot_id` against the
deleted set, and frees blocks of deleted snapshots.

IS_SNAPSHOT_SEG is a hard pin. GC never frees a block carrying this flag
unless its snapshot_id is confirmed deleted in the Snapshot Region.

### Hardlink + Snapshot GC Rule

GC frees the data segments of a hardlinked file (SEG 1, SEG 2, ...) only
when both conditions are true simultaneously:

```
1. All SEG 0 segments sharing this hardlink_id are marked IS_DELETED
AND
2. All snapshots that retained segments of this file_id are deleted
   (is_deleted = 1 in Snapshot Region)
```

Either condition alone is insufficient.

---

## Encryption

**Recommended: Full Disk Encryption** (LUKS, BitLocker, FileVault)
applied underneath ResFS. Recovery is fully unaffected.

**Optional per-segment:** `IS_ENCRYPTED` flag with AES-256-GCM.
Warning: encrypted segments are unrecoverable without the key.
Recovery tool marks them `ENCRYPTED_UNREADABLE`.

Key derivation: `Argon2id(passphrase, salt=file_id)`.
BLAKE3 is computed on plaintext before encryption.
On read: decrypt → verify BLAKE3.

---

## SMI — Segment Map Index

The SMI is the physical acceleration layer: `file_id → extents`.
It is a cache. Destroying it loses nothing — rebuilding from segments
is always possible.

The SMI header also carries `file_counter` and `used_blocks` —
allocation state that belongs to the physical layer.

### SMI Header (4096 bytes, fixed)

```
Offset  Size    Field               Description
------  ----    -----               -----------
0       8       magic               "ResFSSMI"
8       4       version             u32
12      4       reserved            u32, must be 0
16      8       generation          u64, incremented on each IR write
24      8       file_counter        u64, next file_id to assign
32      8       used_blocks         u64, occupied blocks in Data Region
40      8       last_mount          u64, Unix timestamp
48      8       entry_count         u64
56      8       entry_table_offset  u64, relative to SMI start
64      8       extent_pool_offset  u64, relative to SMI start
72      8       reserved            Must be 0
80      32      blake3_root         BLAKE3 of entire SMI body
112     3984    reserved            pad to 4096 bytes
```

### SMI Entry (28 bytes, fixed)

```
Offset  Size    Field               Description
------  ----    -----               -----------
0       8       file_id             u64
8       4       segment_count       u32
12      8       extent_offset       u64, byte offset into extent pool
20      8       extent_count        u64
```

### Extent (24 bytes, fixed)

```
Offset  Size    Field               Description
------  ----    -----               -----------
0       8       start_lba           u64
8       8       length_blocks       u64
16      4       seg_start_index     u32
20      4       seg_count           u32
```

### SMI On-Disk Layout

```
[SMI HEADER 4KB]
[ENTRY TABLE: flat array, entry_count × 28B]
[EXTENT POOL: flat array, addressed via extent_offset]
[DLI HEADER 4KB]
[DLI ENTRY ARRAY: flat array, entry_count × 24B]
[zeroed padding to end of IR region]
```

The start of DLI is computed from SMI header:
```
DLI_start = ir_start + 4096 + entry_count * 28 + extent_count * 24
            (rounded up to next 4096-byte boundary)
```

No separate pointer needed — DLI location is always deterministic from SMI.

### SMI Expansion (within IR)

When SMI grows and approaches DLI:

```
1. Set FS_DIRTY in BH
2. For IR1:
   a. Read DLI entirely into memory
   b. Write expanded SMI (more entries/extents)
   c. Write DLI at new location (after expanded SMI)
   d. Verify BLAKE3 of IR1, increment generation
3. Repeat for IR2
4. Repeat for IR3
5. Clear FS_DIRTY in BH

Power loss during step 2: BLAKE3 of IR1 invalid
  → IR2 and IR3 valid → restore IR1 from them → retry
```

### IR Copy Selection (on mount)

```
1. Read BH at LBA 0 — verify BLAKE3
2. Use BH to locate IR1, IR2, IR3
3. Load SMI header from each IR — verify BLAKE3
4. Discard invalid copies
5. Among valid copies, choose highest generation number
6. Load full SMI + DLI from winning IR — verify full BLAKE3
7. Synchronize remaining IR copies to match winner (lazy, background)
8. Mount
```

### SMI Rebuild (all IR copies invalid)

```
1. Full segment scan of Data Regions
2. Find all segments by magic "ResFSSEG"
3. Verify BLAKE3 of each candidate
4. Filter: IS_COMMITTED not set, IS_DELETED not set
5. Group by file_id
6. Sort each group by seg_index
7. Build extents from contiguous runs
8. max_file_id = max(file_id) across all segments
9. file_counter = max_file_id + 1
10. generation = 1 (or old_max + 1 if any valid partial IR found)
11. Build used_blocks bitmap from occupied LBAs
12. Write new SMI + DLI into all three IR regions
13. Mount
```

---

## DLI — Directory List Index

The DLI is the namespace acceleration layer: `(parent_dir_id, name) → file_id`.
It is a cache. Destroying it loses nothing — rebuilding from directory
segments is always possible.

The DLI is a probabilistic accelerator — it maps name hashes, not full
names. A hash match must always be verified against the directory segment
before a file_id is returned. This verification is the source of truth.

### DLI Header (4096 bytes, fixed)

```
Offset  Size    Field               Description
------  ----    -----               -----------
0       8       magic               "ResFSDLI"
8       4       version             u32
12      4       reserved            u32, must be 0
16      8       generation          u64, same as SMI generation in same IR
24      8       entry_count         u64
32      8       entry_size          u64
40      8       data_offset         u64, relative to DLI start
48      32      blake3_root         BLAKE3 of entire DLI entry array
80      4016    reserved            pad to 4096 bytes
```

### DLI Entry (24 bytes, fixed)

```
Offset  Size    Field               Description
------  ----    -----               -----------
0       8       name_hash           u64 = BLAKE3(name)[0..7]
8       8       parent_dir_id       u64
16      8       file_id             u64
```

### Entry Ordering Rule

DLI entries are always sorted by:
```
sorted_by (parent_dir_id ASC, name_hash ASC)
```

### Lookup Algorithm

Step 1 — compute key:

```c
name_hash = BLAKE3(name)[0..7]
key = (parent_dir_id, name_hash)
```

Step 2 — binary search:

```c
low = 0
high = entry_count - 1

while low <= high:
    mid = (low + high) / 2

    if DLI[mid] < key:
        low = mid + 1
    else if DLI[mid] > key:
        high = mid - 1
    else:
        candidate_file_id = DLI[mid].file_id
        → verify full name in directory segment  ← mandatory
        if name matches: return candidate_file_id
        if name does not match (hash collision):
            linear scan forward/backward for same (parent_dir_id, name_hash)
            verify each candidate against directory segment
            if found: return file_id
            if not found: return ENOENT
```

**DLI lookup never returns a file_id without directory segment verification.**
Collision penalty = one additional directory segment read.

### Entry Comparison Rules

Comparison is lexicographic:

```
if parent_dir_id != other.parent_dir_id:
    compare parent_dir_id

else:
    compare name_hash
```

### DLI Rebuild

DLI is NEVER updated in-place.
It is fully rebuilt from directory segments:

```
1. Scan all directory segments (IS_COMMITTED, not IS_DELETED)
2. Extract entries: (parent_dir_id, name, file_id)
3. Compute: name_hash = BLAKE3(name)[0..7]
4. Build array of DLI entries
5. Sort array by (parent_dir_id, name_hash)
6. Write new DLI
7. Atomically switch IR pointer
```

### Deletion Semantics

Deleted entries are NOT removed immediately.

```
Directory entry marked ENTRY_DELETED in segments
```

During rebuild:

```
Skip ENTRY_DELETED entries
```

### DLI On-Disk Layout

```
[DLI HEADER 4KB]
[DLI ENTRY ARRAY (sorted, contiguous): entry_count × 24B]
[zeroed padding to end of IR region]
```

---

## Index Region Layout

Each Index Region contains exactly one SMI and one DLI.
SMI and DLI within the same IR share the same generation number.
Both are validated together on mount.

All three IRs are always identical in size (`ir_size` from BH).

```
[Index Region N]  (ir_size blocks total)
    [SMI Header 4KB]
    [SMI Entry Table: flat array, entry_count × 28B]
    [SMI Extent Pool: flat array, addressed via extent_offset]
    [DLI Header 4KB]
    [DLI Entry Array: flat array, entry_count × 24B]
    [zeroed padding]
```

DLI start is always computed from SMI header — no separate pointer needed.

### Initial IR Placement (at mkfs)

```
ir_size = max(MIN_IR_BLOCKS, total_blocks * 3 / 1000)
MIN_IR_BLOCKS = 768 (3MB)

IR1: ir1_start = wir_start + wir_size  (immediately after WIR)
IR2: ir2_start = total_blocks / 2
IR3: ir3_start = total_blocks - ir_size - 1 - buffer_blocks
EOP: last_lba  = total_blocks - 1

buffer_blocks = max(1280, total_blocks * 5 / 1000)  (~0.5%, min 5MB)
```

The buffer zones (~0.5% of disk) around each IR are kept free by
the allocator when other free space exists elsewhere. They provide
room for IR expansion without immediate file relocation.

### IR Expansion

IRs expand when SMI + DLI approach capacity. Expansion is sequential —
one IR at a time. While one IR is expanding, the other two serve reads
and writes normally. The expanding IR is temporarily out of rotation.

```
IR1: ir1_start fixed, grows downward into Data Region 1
IR2: ir2_start fixed, grows downward into Data Region 2
IR3: ir3_start moves left (decreases), grows leftward
```

**Expansion algorithm (per IR):**

```
1. Check free_blocks > RESFS_MIN_FREE (1% of total_blocks)
   if not → RESFS_ERR_NO_SPACE, abort

2. Set FS_DIRTY in BH

3. Write new EOP with updated ir_size and new ir3_start
   (EOP updated before BH so that if BH write fails,
    EOP reflects the intended new state)

4. For each IR (one at a time, other two remain active):
   a. Mark IR as expanding (generation set to 0 temporarily)
   b. Relocate files in new IR space via CoW
   c. Write expanded SMI + DLI into new IR region
   d. Verify BLAKE3, restore generation number
   e. IR returns to rotation

5. Update BH: new ir_size + new ir3_start → recompute BLAKE3 → write BH

6. Clear FS_DIRTY in BH

7. GC old segment copies (lazy, background)
```

Power loss before step 3: BH unchanged, EOP unchanged → clean state.
Power loss between step 3 and 5: EOP has new layout, BH has old layout.
On mount: BH valid → use BH. If BH invalid → use EOP hints → IRs intact.
Power loss after step 5: new BH valid → new IRs valid.

**Capacity limits on 1TB disk:**

| ir_size | files supported |
|---------|----------------|
| 0.3% (~3GB) | ~38M files |
| 0.6% (~6GB) | ~76M files |
| 1.2% (~12GB) | ~152M files |

---

## Write Path (normative)

ResFS is a pure CoW filesystem. No journal. No replay.
Power loss at any step leaves the filesystem in a consistent state.

All write operations follow the same pattern:

```
1. Set FS_DIRTY in BH
2. Write WIR entry (file_id, operation, new extents)
3. Write new/changed segments to free blocks
4. fsync
5. Verify BLAKE3 of all written segments
6. Set IS_COMMITTED on SEG 0              ← atomic commit point
7. Update IR copies (one by one, BLAKE3 verify after each)
8. Clear IS_COMMITTED from SEG 0
9. Clear WIR entry
10. Clear FS_DIRTY in BH
```

Power loss before step 6: segments invisible → old version survives intact.
Power loss after step 6, before step 10: IS_COMMITTED on SEG 0 remains.
On next mount: WIR guides scanner to the right blocks → IR updated.

**Aborted write segments** (IS_COMMITTED never set due to crash) are
invisible to the filesystem and will be overwritten by the allocator.
This is correct and expected behavior — these segments were never
committed and carry no recoverable data.

### CREATE

```
1. Set FS_DIRTY in BH
2. counter = atomic_increment(smi.file_counter) → new file_id
3. Write WIR entry (WRITE operation, new extents)
4. Write all segments to free blocks (IS_COMMITTED not set on SEG 0)
5. fsync + verify BLAKE3
6. Set IS_COMMITTED on SEG 0              ← atomic commit point
7. Update SMI: add file_id → extents
8. Write new directory segment (CoW)
9. Update DLI: insert (parent_dir_id, name) → file_id
10. Clear IS_COMMITTED from SEG 0
11. Clear WIR entry
12. Clear FS_DIRTY in BH
```

### WRITE / APPEND

```
1. Set FS_DIRTY in BH
2. Write WIR entry (WRITE operation, new extents)
3. Write new/changed segments to free blocks
4. fsync + verify BLAKE3
5. Set IS_COMMITTED on SEG 0              ← atomic commit point
6. Update SMI extents
7. Clear IS_COMMITTED from SEG 0
8. Clear WIR entry
9. Clear FS_DIRTY in BH

Old segments → GC (lazy, background)
```

### TRUNCATE

```
1. Set FS_DIRTY in BH
2. Update SMI extents (remove extents beyond new size)
3. Update file_size in SEG 0 (via CoW if needed)
4. Set IS_COMMITTED on SEG 0
5. Write updated IR
6. Clear IS_COMMITTED from SEG 0
7. Clear FS_DIRTY in BH

Removed segments → GC (lazy, background)
Truncation is block-level — no zeroing of partial last segment.
```

### DELETE

```
1. Set IS_DELETED on SEG 0               ← atomic commit point
2. Remove DLI entry
3. SMI cleanup deferred to resfs-gc

IS_DELETED on SEG 0 is authoritative — file is deleted.
DLI/SMI stale → rebuilt correctly on next mount.
```

### RENAME

```
1. Set FS_DIRTY in BH
2. Write new directory segment (CoW): old entry tombstoned, new entry added
3. Set IS_COMMITTED on new dir segment   ← atomic commit point
4. Update DLI: remove old entry, insert new entry
5. Clear IS_COMMITTED from dir SEG 0
6. Clear FS_DIRTY in BH

Power loss before step 3: old name survives.
Power loss after step 3: new name committed, DLI rebuilt from dir segments.
```

---

## Free Space

In-memory bitmap of occupied blocks, rebuilt at mount from SMI extents.
Not stored on disk — always reconstructable.

On mount:
```
1. Load SMI extents from winning IR
2. Build in-memory bitmap: mark all extent LBAs as occupied
3. All unmarked blocks → free
4. used_blocks = count of marked blocks
```

On recovery (no valid IR):
```
1. Full segment scan → bitmap of all blocks containing valid segments
2. All unmarked blocks → free
3. Rebuild SMI with correct used_blocks
```

**Allocator strategy:** implementation-defined. Recommendation: prefer
contiguous allocation for large files; pack small files together.

**Fragmentation policy:** if no contiguous region large enough exists
for a file, write into as many extents as available. The file remains
in the defragmenter queue. The defragmenter will consolidate it when
space allows. There is no maximum extent count per file.

---

## GC — Garbage Collection

GC frees blocks occupied by dead segments.
GC is lazy — it never blocks mount or normal operation.
GC loads the Snapshot Region at startup to build the set of live
snapshot_ids before scanning.

### Dead Segment Categories

```
Superseded segments:
  IS_COMMITTED was cleared after CoW write
  → block is occupied but logically free

Deleted file segments:
  SEG 0 has IS_DELETED
  → all segments of this file_id are dead
  → except IS_SNAPSHOT_SEG segments (retained for snapshots)

Aborted write segments:
  IS_COMMITTED was never set (crash during write)
  → block is occupied but invisible to FS
  → allocator may overwrite — this is correct behavior
```

### GC Algorithm (resfs-gc)

```
1. Load Snapshot Region → build set of live snapshot_ids
2. Scan all blocks in Data Region
3. For each block containing "ResFSSEG" magic:
   a. IS_SNAPSHOT_SEG set:
      check snapshot_id against live set
      if snapshot deleted → free block
      if snapshot live → skip (hard pin)
   b. IS_COMMITTED not set AND IS_SNAPSHOT_SEG not set → dead, free block
   c. IS_DELETED on SEG 0:
      scan all segments of this file_id
      for each: IS_SNAPSHOT_SEG not set → dead, free block
4. Update in-memory free bitmap
5. Update used_blocks in SMI header
6. Write updated SMI to all three IR copies
```

### Snapshot GC

```
1. Mark:  set is_deleted = 1 in Snapshot Region entry
          snapshot invisible immediately, no segments touched
2. Sweep: resfs-gc loads Snapshot Region, builds deleted set
          finds all IS_SNAPSHOT_SEG segments with deleted snapshot_id
          frees those blocks
```

GC runs:
- Background daemon (continuous, low priority)
- Explicit: `resfs-gc` invoked by user or system
- Never at mount time (mount is always fast)

---

## Recovery Algorithm

### Normal Mount

```
1. Read BH at LBA 0 — verify BLAKE3
2. Check FS_DIRTY flag:
   if FS_DIRTY=0 → proceed to step 3
   if FS_DIRTY=1 → dirty mount procedure (see below)
3. Locate IR1, IR2, IR3 from BH
4. Load SMI header from each IR — verify BLAKE3
5. Select IR with highest valid generation
6. Load full SMI + DLI from winning IR — verify full BLAKE3
7. Build free space bitmap from SMI extents
8. Mount
```

### Dirty Mount Procedure (FS_DIRTY=1)

```
1. Locate IR1, IR2, IR3 from BH
2. Load SMI header from each IR — verify BLAKE3
3. Select IR with highest valid generation
4. Load full SMI + DLI from winning IR
5. Synchronize other IR copies if needed (lazy, background)
6. Build free space bitmap from SMI extents

7. Read WIR Header at block 1 — verify BLAKE3
   if WIR invalid → fallback to full Data Region scan (step 12)

8. For each WIR entry:
   a. Read listed extent blocks
   b. Check SEG 0 for IS_COMMITTED
   c. IS_COMMITTED=1 → add extents to SMI
   d. IS_COMMITTED=0 → mark blocks free in bitmap
      if operation=DEFRAG → file remains in defrag queue

9. Update all three IR copies with recovered SMI state
10. Clear WIR (zero entry_count, recompute BLAKE3)
11. Clear FS_DIRTY in BH
12. Mount normally

Fallback (WIR invalid):
  Scan all Data Region blocks not present in SMI bitmap
  For each "ResFSSEG" found:
    IS_COMMITTED=1 → add to SMI
    IS_COMMITTED=0 → mark free
  Continue from step 9
```

### BH Lost or Corrupted

```
1. Check EOP at last_lba:
   if EOP valid → use IR and WIR hints from EOP to locate structures
   if EOP invalid → brute-force scan for "ResFSSMI" magic
2. Load SMI headers, verify BLAKE3
3. Select highest valid generation
4. Proceed as normal mount from step 5
5. Optionally reconstruct and rewrite BH at LBA 0
```

### Two-Layer Recovery

```
Layer 1 — valid IR exists:
  Load SMI+DLI from best IR
  Mount immediately
  → seconds

Layer 2 — all IRs destroyed:
  Full O(n) scan of Data Region
  Find all segments by "ResFSSEG" magic
  Group by file_id, sort by seg_index
  Reconstruct files, rebuild SMI+DLI
  → slow but deterministic, always succeeds if segments are intact
```

### Full Disk Scan Algorithm

For each 4096-byte block in Data Region:

```
1. Check "ResFSSEG" magic at offset 0
2. Check "ResFSEND" magic at offset 4072
3. Verify BLAKE3 of data region [64..4071]
4. Validate footer: file_id and seg_index must match header
5. Check IS_COMMITTED — skip if not set
6. Collect valid segment descriptor: (file_id, seg_index, lba, data_len, file_size)
```

After scan:
```
7. Group segments by file_id
8. Sort each group by seg_index
9. Build extents from contiguous LBA runs
10. Reconstruct SMI and DLI
11. Mount
```

Recovery is parallelizable: divide Data Region into stripes,
scan in parallel, merge results by file_id.

### Partial Recovery Policy

```
Gap in chain (seg 5 missing, seg 6+ present):
  → fill gap region with zeros
  → write recovery_info noting missing seg indices
  → file flagged as partially recovered

Orphan segments (valid, file_id not found in any directory):
  → placed in lost+found/
  → filename: resfs_orphan_{file_id}_{seg_index}

Two valid segments at same file_id + seg_index:
  → take segment at lower LBA (deterministic tiebreak)

Encrypted segment, no key:
  → marked ENCRYPTED_UNREADABLE in recovery_info
  → placeholder written in recovered file
```

### Recovery Metadata

```c
struct resfs_recovery_info {
    uint32_t missing_segments;
    uint32_t damaged_segments;
    uint32_t encrypted_unreadable;
    uint32_t reserved;
    uint64_t recovered_bytes;
    uint64_t total_bytes;
};
```

---

## Tooling

| Tool              | Function                                              |
|-------------------|-------------------------------------------------------|
| `mkfs.resfs`      | Format partition: write BH, WIR, IR1/2/3, SR, EOP    |
| `resfs-mount`     | Mount via platform VFS adapter (RhCOS, custom kernel) |
| `resfs-recover`   | Full disk scan → reconstruct all files                |
| `resfs-verify`    | Verify BLAKE3 integrity of all segments               |
| `resfs-visualize` | ASCII visualization of segment and free space layout  |
| `resfs-snap`      | Create, list, restore, delete snapshots               |
| `resfs-gc`        | Garbage collect dead segments and snapshot remnants   |
| `resfs-export`    | Extract raw file or recovery container from ResFS     |
| `resfs-import`    | Import from ext4/NTFS/exFAT/APFS to ResFS            |
| `resfs-label`     | Get or set filesystem label (rewrites BH)             |

### resfs-export modes

```
mode 1 — full recovery:
  resfs-export disk.img movie.mp4
  → extract file, fill gaps with zeros

mode 2 — recovery container:
  resfs-export --recovery disk.img movie.resrecovery
  → extract file + recovery_info (missing/damaged segment map)
  → consumer decides how to handle gaps
```

---

## Target Platforms

| Platform            | Suitable   | Notes                                 |
|---------------------|------------|---------------------------------------|
| Embedded / IoT      | ✅ Primary | Small disks, power loss tolerance     |
| Video recorders     | ✅         | Partial file recovery on power loss   |
| Routers / cameras   | ✅         | Configs, logs, small files            |
| Desktop             | ✅         | CoW makes it fully viable             |
| Server              | ✅         | CoW + snapshots                       |
| Linux               | ✅         | Via platform adapter or kernel module |
| macOS Intel         | ✅         | Via platform adapter                  |
| macOS Apple Silicon | ⚠️          | Requires disabling Secure Boot        |
| Windows             | ✅         | Via WinFSP                            |
| RhCOS               | ✅ Target  | libresfs portable, no OS deps         |

---

## Implementation Roadmap

### Phase 1 — libresfs core

**Criterion: create 10k files, kill -9, recover everything via full scan**

- [ ] `disk.img` creation: BH + WIR + IR1/2/3 + SR + EOP + Data Region
- [ ] Bootstrap Header: read / write / verify BLAKE3
- [ ] WIR Region: read / write / verify BLAKE3 / clear
- [ ] EOP: write / verify / use in recovery
- [ ] Segment: read / write / verify BLAKE3 (header + footer)
- [ ] CoW write path: IS_COMMITTED as atomic gate
- [ ] Root directory: file_id=1, "RESFS ROOT " signature
- [ ] SMI: on-disk format, read, write, rebuild from segments
- [ ] DLI: on-disk format, read, write, rebuild from directory segments
- [ ] IR copy selection: highest valid generation wins
- [ ] IR Expansion: sequential, one IR at a time, others remain active
- [ ] Free space bitmap: build from SMI extents on mount
- [ ] GC: lazy dead segment collection
- [ ] Dirty mount: WIR-guided recovery + fallback full scan
- [ ] Recovery: full disk scan → rebuild SMI+DLI → mount
- [ ] `resfs-recover` proof of concept on disk.img
- [ ] Corruption test suite: random corruption, kill -9, partial writes

### Phase 2 — Advanced Features

- [ ] xattr full implementation
- [ ] ACL full implementation
- [ ] Snapshot management + GC
- [ ] Sparse file full implementation
- [ ] Defragmenter (resfs-defrag):
  - threshold: 8 extents → file in queue
  - queue in memory, rebuilt at mount from SMI
  - background, low priority
  - if file opened for writing during defrag → cancel, move to end of queue
  - if no contiguous space → write fragmented, remain in queue

### Phase 3 — RhK Integration

- [ ] virtio-blk driver (QEMU)
- [ ] resfs_platform_t for RhCOS
- [ ] VFS layer: open/read/write/readdir/close
- [ ] resfs_vfs adapter over libresfs
- [ ] Mount ResFS partition at kernel boot

### Phase 4 — Open Source Release

- [ ] SPEC.md v1.5 finalized ← YOU ARE HERE
- [ ] libresfs complete and tested
- [ ] `resfs-recover` demo: wipe GPT → full recovery
- [ ] Publish `resfs` repo
- [ ] Submit GUID to gdisk partition type database
- [ ] Hacker News / r/osdev launch

---

## Version History

### v0.1
Initial design: segments, file_id (timestamp+rand), CRC32, superblock.

### v0.2
CRC32 → BLAKE3. file_id → UUID v4. AES-256-GCM encryption.
ZSTD compression. Soft/hard delete.

### v0.3
Journaling added. Three-layer recovery model. IS_COMMITTED flag.

### v0.4
Official GUID. Directories as regular files. Symbolic links.
Hard links. Device files. xattr. ACL. Sparse files.
Atomic rename. Snapshots via CoW. Filesystem label.
Flags field expanded to 14 defined bits.

### v0.5
"chunk" → Segment. Magic: ResFSCHK → ResFSSEG.
Superblock Index → SMI. Core Philosophy formalized.
Hard links redesigned: no centralized refcount table.
Directory entries: variable-length format. B+ Tree removed.

### v0.6
Three superblock copies. mime_hash removed.
File ID: UUID v4 → counter+CSPRNG. Inode recycling eliminated.
Snapshot GC: lazy two-phase mark-and-sweep.

### v1.0
BLAKE3 hash field corrected to 32 bytes. Segment data region 3992 → 3976 bytes.
Deduplication removed. DHT introduced. SMI redesigned with extents.
Generation number added. Write path formalized. Conflict resolution rules.
IS_SPARSE_SEG. Partial recovery policy. Orphan segment policy.

### v1.1
Superblock removed (erroneously added, not part of design).
Journal removed — ResFS is pure CoW. Bootstrap Header introduced.
Disk layout corrected: BH + IR1/IR2/IR3 distributed across partition.
Each IR = 1 SMI + 1 DLI. BH carries fs_uuid, fs_label, IR locations.
SMI header carries file_counter, used_blocks. CoW write path formalized.

### v1.2
- file_id: counter+CSPRNG (128-bit) → plain u64 monotonic counter
- seg_total field removed — computable from file_size, was redundant
- file_size = 0 defined for IS_DIRECTORY (size is meaningless)
- Root directory: file_id=1, parent_dir_id=0, "RESFS_ROOT\0" signature
- EOP added: physical boundary marker at last_lba
- IR regions are fixed-size (allocated at mkfs), never grow
- Segment header: 80B → 64B, footer: 40B → 24B
- Data region per segment: 3976B → 4008B (overhead 2.93% → 2.15%)
- GC section formalized: lazy, background, never blocks mount
- Free space model: in-memory bitmap only, not stored on disk
- ACL subject_id: 16B → 8B
- Snapshot region: fixed location after IR1, snap_size in BH

### v1.3
- BH magic: "RESFS PARTITION " (16 bytes)
- BH, SMI, DLI header size: 4096 bytes each
- SMI Entry: 64B → 28B
- Extent: 32B → 24B
- DHT → DLI (Directory List Index) throughout
- DLI redesigned: sorted array + binary search
- DLI Entry: 24B (name_hash + parent_dir_id + file_id)
- IR expansion algorithm introduced (TBD)

### v1.4
- GUID updated: RESFILESYSTEM/AK
- IS_COMMITTED semantics redesigned: only on SEG 0, temporary flag
- FS_DIRTY flag added to BH feature_flags (bit 7)
- CoW write path rewritten: unified algorithm for all operations
- TRUNCATE formalized
- created_at / modified_at: seconds → nanoseconds (u64)
- SMI expansion mechanism: DLI shifts right when SMI grows
- Defragmenter introduced: threshold 8 extents
- FUSE removed from roadmap
- Roadmap restructured: Phase 1 → 2 → 3 → 4

### v1.5
- WIR (Write Intent Record) introduced: new on-disk structure at block 1
  fixed-size pool, wir_size = max(8, total_blocks / 1000)
  replaces brute-force dirty mount scan with targeted block check
  WIR is a recovery accelerator, not a journal — no replay, no redo
  fallback to full scan if WIR invalid
- Disk layout updated: WIR Region added after BH, before IR1
- BH updated: wir_start + wir_size fields added, BLAKE3 range updated
- EOP updated: wir_start + wir_size hints added, BLAKE3 range updated
- Dirty mount procedure formalized: WIR-guided algorithm with fallback
- Write path updated: WIR entry written before CoW, cleared after IR update
- All magic constants unified → ResFSXXX style:
  "ResFsEOP" → "ResFSEOP"
  "SMI0\0\0\0\0" → "ResFSSMI"
  "DLI0\0\0\0\0" → "ResFSDLI"
- Root directory signature: "RESFS_ROOT\0" → "RESFS ROOT "
- DLI collision resolution formalized: hash match requires directory
  segment verification; hash collision → linear scan → verify each
  DLI remains 24 bytes; collision penalty = one additional segment read
- GC + Snapshot interaction formalized:
  IS_SNAPSHOT_SEG is a hard pin — GC loads SR at startup
  GC algorithm updated with explicit snapshot_id liveness check
- Hardlink + Snapshot GC rule added:
  data segments freed only when all SEG 0 with hardlink_id are IS_DELETED
  AND all snapshots retaining those segments are deleted
- IR Expansion moved from Phase 3 to Phase 1
  expansion is sequential: one IR at a time, others remain active
  write operations continue during expansion via remaining two IRs
  IR Expansion crash safety formalized: EOP updated before BH
- Snapshot Region overflow: RESFS_ERR_SNAP_FULL defined
  no automatic eviction — user explicitly deletes via resfs-snap
- file_counter atomicity requirement documented
- Defragmenter fragmentation policy: no maximum extent count,
  write in available space, remain in defrag queue
- Aborted write segments explicitly documented as overwritable

---

*ResFS Specification v1.5*

*Author: Andrei Kovalenko*

*License: PGSL-NC*
