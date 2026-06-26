<img width="1000" alt="ResFS Specification v2.0" src="https://github.com/user-attachments/assets/c5bfe8d6-e965-4581-b919-c014080adac6" />

> Recovery-First Filesystem — every physically intact segment is recoverable, deterministically, without heuristics, even if all metadata is destroyed.

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
    WIA (Write Intent Array)    ← crash recovery accelerator

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
| SMI says file at LBA X, SEG 0 found at LBA Y | SEG 0 wins. SMI rebuilt. |
| DLI says file exists, no SEG 0 found for file_id | File does not exist. DLI entry purged. |
| Two SEG 0 with identical file_id, both IS_COMMITTED | Take segment with higher generation. |
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
Old segment → still valid, still readable
New segment being written  → IS_COMMITTED not yet set → invisible to FS
Power loss here            → old version survives, new never existed for FS
New segment gets IS_COMMITTED → new version visible, old is superseded
```

No journal needed. No replay. Recovery is always deterministic from segments.

### Why WIA instead of a journal?

A journal logs intent and replays operations on recovery. WIA does neither.
WIA only records where new segments were written and the complete current
extent list of the file — so that on recovery, the scanner knows exactly
which blocks to check for IS_COMMITTED, instead of scanning the entire
Data Region.

WIA is a recovery accelerator, not a journal. No replay. No redo.
If WIA itself is corrupt or missing, recovery falls back to full
Data Region scan. The result is always identical — WIA only affects speed.

### Why extents in SEG 0?

SEG 0 is the manifest of the file. It carries the complete list of all
extents belonging to the current version of the file. This makes full disk
scan trivial: find SEG 0 with IS_COMMITTED → read extents → file reconstructed.

No separate index lookup required. Segments are self-describing.

### Why file_id is a plain u64 counter?

Simple, small, and recoverable. At 1,000,000 new files per second a u64
counter overflows in ~585,000 years — more than sufficient for any use case.

On recovery without valid IR: scan all segments, find the maximum file_id
value, resume from max+1. Trivially O(n), no set membership problem.

Deleted file_ids are never recycled.

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
  Block 1..wia_end      WIA Region — fixed size, allocated at mkfs
  Block wia_end+1..     Snapshot Region (SR) — fixed size, allocated at mkfs
  Block sr_end+1..      Index Region 1 (IR1) — initial size, expandable
  Block ir1_end+1..     IR1 expansion buffer
  Block buf1_end+1..    Data Region 1 (segments)
  [partition midpoint]  Index Region 2 (IR2) — initial size, expandable
  Block ir2_end+1..     IR2 expansion buffer
  [continued]           Data Region 2 (segments, continued)
  [near end]            IR3 expansion buffer
  Block buf3_end+1..    Index Region 3 (IR3) — initial size, expandable
  [last block]          End Of Partition segment (EOP)
```

**WIA Region is fixed-size**, allocated at mkfs immediately after BH.
Its location is always known: block 1. No pointer needed.

**Index Regions have initial size** defined at mkfs. They expand into
adjacent expansion buffers when SMI + DLI approach capacity.
IR1 and IR2 expand downward (into Data Region). IR3 expands leftward.

**Expansion buffers** are initially part of the Data Region. The block
allocator avoids writing data into them unless no other free space exists.
Any data present in a buffer is relocated via COW_EXPAND before IR expansion.

Three Index Regions distributed across the partition:
- **IR1**: immediately after Snapshot Region
- **IR2**: at partition midpoint
- **IR3**: near partition end (right before EOP)

Each IR = 1 SMI + 1 DLI. Three IRs = three independent copies of SMI+DLI,
physically separated. BH carries the exact LBA of all three IRs.

---

## Bootstrap Header (BH)

The BH carries partition identity and structural layout pointers.
It is not a superblock — it holds no file metadata, no counters,
and no mount history.

All BH fields except `ir_size` and `ir3_start` are written once at mkfs
and remain stable for the lifetime of the partition. `ir_size` and
`ir3_start` are updated only on IR expansion.

```
Offset  Size    Field              Description
------  ----    -----              -----------
0       16      BH_SIG             Magic: "RESFS PARTITION "
16      1       version_major      u8
17      1       version_minor      u8
18      2       version_patch      u16
20      4       block_size         u32, always 4096
24      16      fs_uuid            UUID of this filesystem instance
40      1       label_len          u8, length of fs_label
41      255     fs_label           UTF-8 filesystem label, null-padded
296     4       feature_flags      u32, see Feature Flags section
300     8       wia_start          u64, LBA of WIA Region (always 1)
308     8       wia_size           u64, size of WIA Region in blocks
316     8       sr_start           u64, LBA of Snapshot Region
324     8       sr_size            u64, size of Snapshot Region in blocks
332     8       ir1_start          u64, LBA of IR1 (fixed at mkfs, never changes)
340     8       ir2_start          u64, LBA of IR2 (fixed at mkfs, never changes)
348     8       ir3_start          u64, LBA of IR3 (moves left on expansion)
356     8       ir_size            u64, current size of each IR in blocks (grows on expansion)
364     8       data1_start        u64, LBA of Data Region 1
372     8       data2_start        u64, LBA of Data Region 2
380     8       start_of_partition u64, Absolute LBA of the first block
388     8       partition_size     u64, Partition size in blocks
396     32      blake3_hash        BLAKE3 of bytes [0..395]
428     3668    reserved           Must be zero (pad to 4096 bytes)
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
6-31    Reserved, must be 0
```

### Initial Layout Calculation (at mkfs)

```
wia_size  = max(MIN_WIA_BLOCKS, total_blocks / 1000)
MIN_WIA_BLOCKS = 8 (32KB)

ir_size   = max(MIN_IR_BLOCKS, total_blocks * 3 / 1000)
MIN_IR_BLOCKS = 768 (3MB)

buffer_blocks = max(1280, total_blocks * 5 / 1000)  (~0.5%, min 5MB)

wia_start  = 1
sr_start   = wia_start + wia_size
ir1_start  = sr_start + sr_size
data1_start = ir1_start + ir_size + buffer_blocks
ir2_start  = total_blocks / 2
data2_start = ir2_start + ir_size + buffer_blocks
ir3_start  = total_blocks - ir_size - 1
EOP: last_lba = total_blocks - 1
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
0       8       EOP_SIG            Magic: "ResFSEOP"
8       1       version_major      u8
9       1       version_minor      u8
10      2       version_patch      u16
12      4       block_size         u32, always 4096
16      16      fs_uuid            UUID of this filesystem instance
32      1       label_len          u8, length of fs_label
33      255     fs_label           UTF-8 filesystem label, null-padded
288     4       feature_flags      u32, see Feature Flags section
292     8       wia_start          u64, LBA of WIA Region (always 1)
300     8       wia_size           u64, size of WIA Region in blocks
308     8       sr_start           u64, LBA of Snapshot Region
316     8       sr_size            u64, size of Snapshot Region in blocks
324     8       ir1_start          u64, LBA of IR1
332     8       ir2_start          u64, LBA of IR2
340     8       ir3_start          u64, LBA of IR3
348     8       ir_size            u64, current size of each IR in blocks
356     8       data1_start        u64, LBA of Data Region 1
364     8       data2_start        u64, LBA of Data Region 2
372     8       start_of_partition u64, Absolute LBA of the first block
380     8       partition_size     u64, Partition size in blocks
388     32      blake3_hash        BLAKE3 of bytes [0..387]
420     3654    reserved           Must be zero
4074    22      EOP_TAIL           "END OF RESFS PARTITION"
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
     verify BLAKE3([0..387])
     if valid and fs_uuid matches → EOP found

3. EOP found:
   start_of_partition → partition start
   partition_size     → partition end
   wia_start/wia_size → hints to locate WIA
   ir1/ir2/ir3_start  → hints to locate IR copies
   data1_start / data2_start → hints for Data Regions

4. Attempt to load BH from start_of_partition:
   if BH valid → use all locations from BH (authoritative)
   if BH invalid → use hints from EOP

5. Attempt to load IR copies:
   if valid IR found → mount with SMI+DLI
   if no valid IR → full segment scan from data1_start to partition end

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

---

## WIA — Write Intent Array

The WIA Region is a fixed-size array of write intent entries, allocated
at mkfs. It lives at block 1 — immediately after BH — and is always
findable without any pointer.

WIA is a crash recovery accelerator. Before any CoW operation, a WIA entry
is written recording the file_id, operation type, and the complete current
extent list of the file (including new extents). On recovery, the scanner
reads WIA and checks only those specific blocks for IS_COMMITTED.

**WIA is not a journal.** No operations are replayed. No redo log.
If IS_COMMITTED is not set on SEG 0, the write is simply discarded —
the old version survives in SMI. WIA only tells the scanner where to look
and what the intended new state was.

If WIA itself is corrupt or absent on recovery, the fallback is a full
Data Region scan. The result is always identical — WIA only affects speed.

**WIA overflow:** if entry_count reaches capacity, all new write operations
block until IR is updated and WIA is cleared. This is extremely rare —
capacity scales with disk size and each entry covers a complete file
(≤ 8 extents × 20B per file due to defragmenter threshold).

### WIA Header (4096 bytes, fixed)

```
Offset  Size    Field           Description
------  ----    -----           -----------
0       8       WIA_SIG         Magic: "ResFSWIA"
8       4       reserved        u32, must be 0
12      8       generation      u64, incremented on each WIA write
20      8       entry_count     u64, number of active entries
28      8       capacity        u64, maximum entries (computed from wia_size)
36      8       data_offset     u64, byte offset to entry array (relative to WIA start)
44      32      blake3_hash     BLAKE3 of entire WIA body (header + entries)
76      4020    reserved        pad to 4096 bytes
```

### WIA Entry (variable length)

```
Offset  Size            Field       Description
------  ----            -----       -----------
0       8               file_id     u64
8       1               operation   u8, see WIA Operations
9       3               reserved    must be 0
12      4               ext_count   u32, number of extents
16      ext_count × n   extents     complete current extent list of the file
```

### WIA Operations

```
CREATE  = 1    new file creation
WRITE   = 2    CoW rewrite (partial or full)
DEFRAG  = 3    defragmenter CoW relocation
EXPAND  = 4    IR expansion CoW relocation
```

### WIA Write Path

WIA entry is written before any CoW operation begins:

```
1. Write WIA entry (file_id, operation, complete extents)
2. Verify BLAKE3 of WIA
3. Proceed with CoW operation
```

Crash during step 1: WIA BLAKE3 invalid → WIA entry ignored on recovery.
No segments have been written yet → filesystem is consistent.

Crash after step 1: WIA entry is valid → recovery reads it → checks
only the listed blocks for IS_COMMITTED.

### WIA on Recovery

```
1. Read WIA Header — verify BLAKE3
   if invalid → fallback to full Data Region scan
2. For each WIA entry:
   a. Read SEG 0 LBA from WIA extents (seg_index = 0)
   b. Check SEG 0 for IS_COMMITTED
   c. IS_COMMITTED=1 → write was committed, IR not yet updated
      → add all extents from WIA entry to SMI
   d. IS_COMMITTED=0 → write was aborted
      → mark new blocks free in bitmap (blocks listed in WIA but not in SMI)
      → if operation=DEFRAG → file remains in defrag queue
3. Update all three IR copies with recovered SMI state
4. Clear WIA (zero entry_count, recompute BLAKE3)
```

### WIA Capacity

```
capacity = (wia_size * 4096 - 4096) / avg_entry_size
```

On a 1TB disk (wia_size ≈ 256 blocks): capacity >> 1000 entries.
On a 64MB embedded disk (wia_size = 8 blocks, 32KB): capacity ≈ 100 entries.

WIA capacity scales automatically with disk size. No configuration needed.

---

## Snapshot Region (SR)

The SR is a fixed-size table located immediately after WIA. Each SR entry
records a snapshot and points to its snapshot file in the Data Region.

### SR Header (4096 bytes, fixed)

```
Offset  Size    Field               Description
------  ----    -----               -----------
0       8       SR_SIG              Magic: "ResFSSNP"
8       8       snapshot_counter    u64, monotonic counter for snapshot_id assignment
16      8       entry_count         u64, number of entries (live + deleted)
24      4072    reserved            pad to 4096 bytes
```

`live_count` is an in-memory value only. It is computed at mount by counting SR entries where `live == 1`. It is never written to disk.

### SR Entry (fixed size)

```
Offset  Size    Field               Description
------  ----    -----               -----------
0       8       snapshot_id         u64, unique monotonic ID
8       8       snap_file_id        u64, file_id of the snapshot file in Data Region
16      8       created_at          u64, Unix timestamp nanoseconds
24      1       live                u8, 1 = live, 0 = deleted
25      3       reserved            must be 0
28      32      blake3_hash         BLAKE3 of bytes [0..59]
```

### Snapshot Files

Each live snapshot owns a snapshot file in the Data Region, referenced by
`snap_file_id`. The snapshot file is a regular file with `IS_SNAPSHOT_FILE`
flag. Its data region contains a flat array of extent entries:

```
Offset  Size    Field               Description
------  ----    -----               -----------
0       8       start_lba           u64
8       8       length_blocks       u64
16      4       seg_index           u32
20      4       reserved            u32, must be 0
24      8       file_id             u64, file this extent belongs to
32      8       created_at          u64, Unix timestamp nanoseconds of the segment
```

Each entry describes a block that was superseded by a CoW write after
the snapshot was created and is retained for snapshot read access.

### Snapshot Ownership Model

Each superseded segment carries a `snapshot_id` field identifying the
newest live snapshot for which `snapshot.created_at > segment.created_at`.
This is determined at the time the segment is superseded.

At mount, the bitmap is built from SMI extents plus all extents listed
in snapshot files of live snapshots, preventing the allocator from
overwriting retained blocks.

If the SR is full, `RESFS_ERR_SNAP_FULL` is returned. The user must
explicitly delete a snapshot via `resfs-snap` before creating a new one.
Snapshots are never deleted automatically.

---

## SMI — Segment Map Index

The SMI is the physical acceleration layer: `file_id → SEG 0 location`.
It is a cache. Destroying it loses nothing — rebuilding from segments
is always possible.

The SMI header also carries `file_counter` and `used_blocks` —
allocation state that belongs to the physical layer.

### SMI Header (4096 bytes, fixed)

```
Offset  Size    Field               Description
------  ----    -----               -----------
0       8       SMI_SIG             "ResFSSMI"
8       8       reserved            u64, must be 0
16      8       generation          u64, incremented on each IR write
24      8       file_counter          u64, next file_id to assign
32      8       used_blocks         u64, occupied blocks in Data Region
40      8       last_mount          u64, Unix timestamp nanoseconds
48      8       entry_count         u64
56      32      blake3_hash         BLAKE3 of entire SMI body
88      4008    reserved            pad to 4096 bytes
```

### SMI Entry (16 bytes, fixed)

```
Offset  Size    Field               Description
------  ----    -----               -----------
0       8       file_id             u64
8       8       seg0_lba            u64, LBA of SEG 0 for this file
```

SMI maps file_id to the physical location of SEG 0. All extent information
is read directly from SEG 0. No extent pool — segments are self-describing.

### SMI On-Disk Layout

```
[SMI HEADER 4KB]
[SMI ENTRY ARRAY: flat array, entry_count × 16B, sorted by file_id]
[DLI HEADER 4KB]
[DLI ENTRY ARRAY: flat array, entry_count × 24B]
[zeroed padding to end of IR region]
```

SMI entries are sorted by `file_id` for binary search lookup.

### SMI Lookup

```
Binary search on file_id → seg0_lba
Read SEG 0 at seg0_lba → extents, file_size, flags
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
2. Find all SEG 0 by "ResFSSEG" magic + IS_FIRST_SEG flag
3. Verify BLAKE3 of each candidate
4. Filter: IS_COMMITTED not set, IS_DELETED not set
5. For duplicate file_id: take SEG 0 with higher generation
6. Build SMI entries: file_id → seg0_lba
7. max_file_id = max(file_id) across all valid SEG 0
8. file_counter = max_file_id + 1
9. generation = 1 (or old_max + 1 if any valid partial IR found)
10. Build used_blocks bitmap from SMI + SR extents
11. Write new SMI + DLI into all three IR regions
12. Mount
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
0       8       DLI_SIG             "ResFSDLI"
8       8       reserved            u64, must be 0
16      8       generation          u64, same as SMI generation in same IR
24      8       entry_count         u64
32      8       data_offset         u64, relative to DLI start
40      32      blake3_hash         BLAKE3 of entire DLI entry array
72      4024    reserved            pad to 4096 bytes
```

### DLI Entry (24 bytes, fixed)

```
Offset  Size    Field               Description
------  ----    -----               -----------
0       8       name_hash           u64 = BLAKE3(name)[0..7]
8       8       file_id             u64
16      8       parent_dir_id       u64
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

### DLI Rebuild

DLI is NEVER updated in-place.
It is fully rebuilt from directory segments:

```
1. Scan all directory segments (IS_COMMITTED not set, IS_DELETED not set)
2. Extract entries: (parent_dir_id, name, file_id)
3. Compute: name_hash = BLAKE3(name)[0..7]
4. Build array of DLI entries
5. Sort array by (parent_dir_id ASC, name_hash ASC)
6. Write new DLI
```

---

## Index Region Layout

Each Index Region contains exactly one SMI and one DLI.
SMI and DLI within the same IR share the same generation number.
Both are validated together on mount.

```
[Index Region N]  (ir_size blocks total)
    [SMI Header 4KB]
    [SMI Entry Array: flat array, entry_count × 16B, sorted by file_id]
    [DLI Header 4KB]
    [DLI Entry Array: flat array, entry_count × 24B]
    [zeroed padding]
```

All three IRs are always identical in layout and content (same generation).
IR copies diverge only during write operations and are re-synchronized
immediately after.

### IR Update Order

IR copies are always written in strict FIFO order: IR1 → IR2 → IR3.
After each write, BLAKE3 is verified. This guarantees at least one IR copy
always contains consistent metadata even during a crash mid-update.

### IR Expansion

IRs expand when SMI + DLI approach capacity. Expansion is sequential —
one IR at a time. While one IR expands, the other two serve reads and
writes normally. The expanding IR is temporarily out of rotation.

```
IR1: ir1_start fixed, expands downward into Data Region 1
IR2: ir2_start fixed, expands downward into Data Region 2
IR3: ir3_start moves left (decreases), expands leftward
```

**Expansion algorithm (per IR):**

```
1. Check free_blocks > RESFS_MIN_FREE (1% of total_blocks)
   if not → RESFS_ERR_NO_SPACE, abort

2. Write new EOP with updated ir_size and new ir3_start
   (EOP updated before BH so that if BH write fails,
    EOP reflects the intended new state)

3. For each file in expansion buffer zone:
   a. COW_EXPAND: relocate file to free Data Region space
   b. Update SMI: seg0_lba → new location

4. For each IR (one at a time, others remain active):
   a. Mark IR as expanding (generation set to 0 temporarily)
   b. Write expanded SMI + DLI into new IR region
   c. Verify BLAKE3, restore generation number
   d. IR returns to rotation

5. Update BH: new ir_size + new ir3_start → recompute BLAKE3 → write BH
```

Power loss before step 2: BH unchanged, EOP unchanged → clean state.
Power loss between step 2 and 5: EOP has new layout, BH has old layout.
On mount: BH valid → use BH. If BH invalid → use EOP hints → IRs intact.
Power loss after step 5: new BH valid → new IRs valid.

**Capacity on 1TB disk:**

| ir_size | files supported |
|---------|----------------|
| 0.3% (~3GB) | ~200M files |
| 0.6% (~6GB) | ~400M files |
| 1.2% (~12GB) | ~800M files |

---

## Segment Layout (4096 bytes total)

```
Offset  Size    Field           Description
------  ----    -----           -----------

=== HEADER (88 bytes) ===

0       8       SEG_SIG         Magic: "ResFSSEG"
8       8       file_id         u64, monotonic counter
16      4       seg_index       u32, position in file (0-based)
20      4       flags           u32, see Flags section
24      4       data_len        u32, actual bytes in data region
28      4       reserved        Must be 0x00000000
32      8       file_size       u64, total file size in bytes (0 for IS_DIRECTORY)
40      8       snapshot_id     u64, newest live snapshot retaining this block (0 if none)
48      8       created_at      u64, Unix timestamp nanoseconds of this segment write
56      32      blake3_hash     BLAKE3-256 of data region [88..4071]

=== DATA REGION (3984 bytes) ===

88      3984    data            Raw file data (or SEG 0 layout for IS_FIRST_SEG)

=== FOOTER (24 bytes) ===

4072    8       SEG_END_SIG     Magic: "ResFSEND"
4080    8       file_id         u64, repeated for footer validation
4088    4       seg_index       u32, repeated for footer validation
4092    4       reserved        Must be 0x00000000
```

**Overhead: 112 bytes / 4096 = 2.73%**

---

## Flags Field

```
Bit     Meaning
---     -------
0       IS_FIRST_SEG        seg_index == 0, data starts with SEG 0 layout
1       IS_LAST_SEG         final segment, data_len may be < 4000
2       IS_COMPRESSED       data is ZSTD compressed
3       IS_ENCRYPTED        data is AES-256-GCM encrypted
4       IS_INLINE           SEG 0 only: file data stored inline in SEG 0
5       IS_DELETED          soft delete marker (SEG 0 only)
6       IS_COMMITTED        CoW commit gate — set after all segments written, before IR update
7       IS_DIRECTORY        this file is a directory
8       IS_SYMLINK          this file is a symbolic link
9       IS_DEVICE_FILE      this file is a device file
10      IS_SPARSE_SEG       placeholder for sparse hole (data_len = 0)
11      IS_XATTR_SEG        this segment contains extended attributes
12      IS_ACL_SEG          this segment contains ACL entries
13      IS_SNAPSHOT_FILE    this file is a snapshot extent list (owned by SR)
14      EXT_OVERFLOW        SEG 0 only: extent count exceeds 188, file is orphaned on full scan
15-31   Reserved, must be 0
```

**IS_COMMITTED is the CoW commit gate. It lives only on SEG 0.**

IS_COMMITTED means: "all new segments have been written but IR has not yet
been updated." It is a temporary flag, not a permanent attribute.

Normal state of all files: IS_COMMITTED is NOT set.
IS_COMMITTED is set only during the window between segment write and IR update.

---

## SEG 0 Layout

SEG 0 is the manifest of the file. It carries file metadata, the complete
list of all extents of the current file version, and optionally inline data
for small files. SEG 0 is 4096 bytes total (one block).

### SEG 0 Header (312 bytes)

```
Offset  Size    Field           Description
------  ----    -----           -----------
88      1       filename_len    u8, length of filename in bytes
89      255     filename        UTF-8, null-padded
344     4       permissions     Unix rwxrwxrwx + setuid/setgid/sticky (12 bits)
348     4       reserved        Must be 0x00000000
352     8       generation      u64, incremented on each CoW rewrite of SEG 0
360     8       modified_at     u64, Unix timestamp nanoseconds (last write)
368     8       owner_uid       u64
376     8       owner_gid       u64
384     8       hardlink_id     u64, shared file_id if hardlink, else 0
```

Total header: offset 88 to 392 = 304 bytes. With segment header (88B): 392B.

### IS_INLINE = 1 (small files, ≤ 3680 bytes)

```
[Segment header: 88B]
[SEG 0 header: 304B]
[inline data: 3680B]
[Segment footer: 24B]
```

File data is stored directly in SEG 0. No additional blocks allocated.
Total = 4096 bytes. One block per small file.

When a file grows beyond 3688 bytes, it is rewritten via COW_WRITE:
new SEG 0 without IS_INLINE + data in separate segments.

### IS_INLINE = 0 (regular files)

```
[Segment header: 88B]
[SEG 0 header: 304B]
[reserved: 12B]
[extent_count: u64, 8B]
[extents: extent_count × 20B]
[zeroed padding to footer]
[Segment footer: 24B]
```

Maximum extents: (4096 - 88 - 304 - 8 - 24) // 20 = 3668 // 20 = **183 extents**.

If extent_count > 183: set EXT_OVERFLOW flag. On full disk scan,
file is treated as orphaned — all physically present segments are collected
but correctness is not guaranteed. This requires extreme fragmentation
(defragmenter threshold is 8 extents) and is practically impossible
under normal operation.

---

## Extent (20 bytes, fixed)

```
Offset  Size    Field               Description
------  ----    -----               -----------
0       8       start_lba           u64
8       8       length_blocks       u64
16      4       seg_index           u32, position of the segment
```

Extents describe contiguous runs of segments on disk. `seg_index` identifies
which logical position in the file this extent starts at, allowing the reader
to assemble the file in correct order regardless of physical layout.

---

## File ID

```c
typedef uint64_t file_id_t;
```

File IDs are plain monotonic u64 counters. Simple, small, recoverable.

- At 1,000,000 new files/second: overflow in ~585,000 years
- On recovery without valid IR: scan all SEG 0 segments, take max(file_id) + 1
- Deleted file_ids are never recycled

The current counter value (`file_counter`) lives in the SMI header.
In memory: incremented atomically on every CREATE. Implementation must
guarantee atomicity — the mechanism is platform-defined.
On disk: written to IR on checkpoint and unmount.
On crash: recovered from max(file_id) across all committed SEG 0 segments.

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
flags          = IS_FIRST_SEG | IS_DIRECTORY | IS_COMMITTED
filename       = "/"
data[0..10]    = "RESFS ROOT "   (signature at start of inline data)
```

The root directory signature `"RESFS ROOT "` precedes directory entries
in the data region of SEG 0.

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
Average entry (~25-char name): ~38 bytes.

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

Reference count is implicit: count of live (not IS_DELETED) SEG 0 segments
sharing the same `hardlink_id`. On recovery: scan → count → determine liveness.

---

## Device Files

Device files use `IS_DEVICE_FILE` flag. SEG 0 data region contains
(after the standard file header):

```
Offset  Size    Field
------  ----    -----
384     4       major_number    u32
388     4       minor_number    u32
392     8       driver_id       u64
400     4       device_flags    u32
404     4       device_type     u32
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

## Sparse Files

A sparse segment is a placeholder for a hole — a zero region with no
physical data. `IS_SPARSE_SEG` set → `data_len = 0`.

The scanner finds sparse segments and maps their `seg_index` positions
as zero regions. Physical space usage is proportional to actual content.

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

## Write Path (normative)

ResFS is a pure CoW filesystem. No journal. No replay.
Power loss at any step leaves the filesystem in a consistent state.

### CREATE

```
1. file_id = atomic_increment(smi.file_counter)
2. Check WIA: if entry with this file_id exists → wait/error
3. Allocate blocks in bitmap (in memory)
4. Write WIA entry (file_id, CREATE, all extents)
5. Write all segments with created_at = now()
6. Compute BLAKE3, verify
7. Set IS_COMMITTED on SEG 0
8. Update SMI: add {file_id, seg0_lba}
9. Update DLI: add {name_hash, file_id, parent_dir_id}
10. Write IR1 → IR2 → IR3
11. Clear IS_COMMITTED on SEG 0
12. Remove WIA entry
```

### COW_WRITE

```
1. Check WIA: if entry with this file_id exists → wait/error
2. Allocate new blocks in bitmap
3. Write WIA entry (file_id, WRITE, all new extents)
4. Write new segments with created_at = now()
5. Write new SEG 0 with generation+1, new extents
6. Compute BLAKE3, verify
7. Set IS_COMMITTED on new SEG 0
8. Update SMI: seg0_lba → new address
9. If sr.live_count == 0:
     Clear old blocks in bitmap
   If sr.live_count > 0:
     For each old superseded segment:
       If newest_live_snapshot.created_at > segment.created_at:
         Set segment.snapshot_id = newest_live_snapshot.snapshot_id
         Append extent to that snapshot's snap file
       Else:
         Clear block in bitmap
10. Write IR1 → IR2 → IR3
11. Clear IS_COMMITTED
12. Remove WIA entry
```

### COW_DEFRAG

```
1. Check WIA: if entry with this file_id exists → wait/error
2. Allocate new contiguous blocks in bitmap
3. Write WIA entry (file_id, DEFRAG, all new extents)
4. Copy data to new blocks, preserve created_at of each segment
5. Compute BLAKE3, verify
6. Write new SEG 0 with generation+1, new extents
7. Set IS_COMMITTED on new SEG 0
8. Update SMI: seg0_lba → new address
9. If sr.live_count == 0:
     Clear old blocks in bitmap
   If sr.live_count > 0:
     For each old superseded segment:
       If newest_live_snapshot.created_at > segment.created_at:
         Set segment.snapshot_id = newest_live_snapshot.snapshot_id
         Append extent to that snapshot's snap file
       Else:
         Clear block in bitmap
10. Write IR1 → IR2 → IR3
11. Clear IS_COMMITTED
12. Remove WIA entry
```

### COW_EXPAND

```
1. Find all files whose segments reside in expansion buffer zone
2. For each such file: execute COW_DEFRAG to free Data Region space
3. Expansion buffer is now free → expand IR into it
4. Write IR1 → IR2 → IR3 with new ir_size
5. Update BH and EOP with new ir_size and ir3_start
```

### TRUNCATE

```
1. Check WIA: if entry with this file_id exists → wait/error
2. Find SEG 0 via SMI (seg0_lba)
3. Read extents from SEG 0
4. Determine segments beyond new_size
5. Allocate new block for new SEG 0 in bitmap
6. Write WIA entry (file_id, WRITE, new extents)
7. Write new SEG 0 with generation+1, trimmed extents, updated file_size
8. Compute BLAKE3, verify
9. Set IS_COMMITTED on new SEG 0
10. Update SMI: seg0_lba → new address
    if file_id > smi.file_counter: smi.file_counter = file_id + 1
11. If sr.live_count == 0:
      Clear removed blocks + old SEG 0 block in bitmap
    If sr.live_count > 0:
      For each removed segment:
        If newest_live_snapshot.created_at > segment.created_at:
          Set segment.snapshot_id = newest_live_snapshot.snapshot_id
          Append extent to that snapshot's snap file
        Else:
          Clear block in bitmap
      Clear old SEG 0 block in bitmap
12. Write IR1 → IR2 → IR3
13. Clear IS_COMMITTED
14. Remove WIA entry
```

### DELETE

```
1. Check WIA: if entry with this file_id exists → wait/error
2. Find SEG 0 via SMI (seg0_lba)
3. If sr.live_count == 0:
     Clear all file blocks in bitmap
   If sr.live_count > 0:
     For each segment of this file:
       If newest_live_snapshot.created_at > segment.created_at:
         Set segment.snapshot_id = newest_live_snapshot.snapshot_id
         Append extent to that snapshot's snap file
       Else:
         Clear block in bitmap
     Set IS_DELETED on SEG 0
4. Remove from DLI
5. Remove from SMI
6. Write IR1 → IR2 → IR3
```

### RENAME

```
1. Find file_id via DLI
2. COW_WRITE old parent directory segment: remove old entry
3. COW_WRITE new parent directory segment: add new entry with new name
   (if old and new parent are the same directory: steps 2 and 3 merge into one COW_WRITE)
4. Update DLI entry in-place: new name_hash, new parent_dir_id
5. Write IR1 → IR2 → IR3
```

### SNAPSHOT_CREATE

```
1. snapshot_id = atomic_increment(sr.snapshot_counter)
2. Create snap file via CREATE (IS_SNAPSHOT_FILE flag, empty)
3. Write SR entry: {snapshot_id, snap_file_id, label, created_at=now(), live=1}
4. Increment sr.live_count in memory
5. Write IR1 → IR2 → IR3
```

### SNAPSHOT_DELETE

```
1. Find SR entry by snapshot_id
2. Find snap file via SMI (snap_file_id → seg0_lba)
3. Read all extents from snap file
4. Find previous live snapshot (prev_snap) in SR by created_at
5. For each block in snap file:
   If prev_snap exists AND prev_snap.created_at > block.created_at:
     Append block to prev_snap's snap file
     Update block's snapshot_id to prev_snap.snapshot_id
   Else:
     Clear block in bitmap
6. DELETE snap file (regular DELETE operation)
7. Set SR entry live = 0
8. Decrement sr.live_count in memory
9. Write IR1 → IR2 → IR3
```

---

## Free Space

In-memory bitmap of occupied blocks, rebuilt at every mount.
Not stored on disk — always reconstructable.

### Bitmap Construction at Mount

```
1. Load SMI entry array from winning IR
2. For each SMI entry:
   a. Read SEG 0 at seg0_lba
   b. If IS_INLINE: mark seg0_lba as occupied
   c. If not IS_INLINE: mark seg0_lba + all extents as occupied
3. For each live snapshot in SR:
   a. Read snap file via SMI (snap_file_id → SEG 0 → extents)
   b. Mark all extents in snap file as occupied
4. All unmarked blocks → free
5. used_blocks = count of marked blocks
```

### Block Allocator Strategy

The allocator prefers free space in Data Regions over expansion buffers.
Expansion buffers are used only when no other free space exists.
Recommendation: prefer contiguous allocation for large files;
pack small files together.

---

## Recovery Algorithm

### Mount

```
1. Read BH at LBA 0 — verify BLAKE3
2. Locate IR1, IR2, IR3 from BH
3. Load SMI header from each IR — verify BLAKE3
4. Select IR with highest valid generation
5. Load full SMI + DLI from winning IR — verify full BLAKE3
6. Synchronize other IR copies if needed (lazy, background)

7. Read WIA Header at block 1 — verify BLAKE3
   if WIA entry_count == 0 → skip to step 12 (clean mount)
   if WIA invalid → fallback to full Data Region scan (step 13)

8. For each WIA entry:
   a. Read SEG 0 LBA from WIA extents
   b. Check SEG 0 for IS_COMMITTED
   c. IS_COMMITTED=1 → add extents to SMI, update seg0_lba
      → if file_id > smi.file_counter: smi.file_counter = file_id + 1
   d. IS_COMMITTED=0 → mark new blocks free in bitmap

9. Update all three IR copies with recovered SMI state
10. Clear WIA (zero entry_count, recompute BLAKE3)
11. Load SR — compute sr.live_count = count(SR entries where live == 1)
12. Build free space bitmap (SMI + SR, see Bitmap Construction)
13. Mount

Fallback (WIA invalid):
  Scan all Data Region blocks not present in SMI bitmap
  For each "ResFSSEG" found:
    IS_COMMITTED=1 → add to SMI
    IS_COMMITTED=0 → mark free
  Continue from step 9
```

### BH Lost or Corrupted

```
1. Check EOP at last_lba:
   if EOP valid → use IR and WIA hints from EOP to locate structures
   if EOP invalid → brute-force scan for "ResFSSMI" magic
2. Load SMI headers, verify BLAKE3
3. Select highest valid generation
4. Proceed as normal mount from step 4
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
  Find all SEG 0 by "ResFSSEG" magic
  Group by file_id, take highest generation per file_id
  Reconstruct files, rebuild SMI+DLI
  → slow but deterministic, always succeeds if segments are intact
```

### Full Disk Scan Algorithm

For each 4096-byte block in Data Region:

```
1. Check "ResFSSEG" magic at offset 0
2. Check "ResFSEND" magic at offset 4072
3. Verify BLAKE3 of data region [88..4071]
4. Validate footer: file_id and seg_index must match header
5. If IS_FIRST_SEG (SEG 0):
   a. Check IS_COMMITTED
   b. If IS_COMMITTED → record (file_id, generation, lba, extents from SEG 0)
   c. If not IS_COMMITTED → skip (aborted write)
   d. If EXT_OVERFLOW → record as orphaned
6. If not IS_FIRST_SEG:
   a. Record (file_id, seg_index, lba) for assembly after scan
```

After scan:

```
7. For each file_id: select SEG 0 with highest generation
8. Read extents from winning SEG 0
9. Verify data segments are present at expected LBAs
10. Reconstruct SMI and DLI
11. Handle IS_DELETED files: present in scan but excluded from DLI
12. Mount
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
  → filename: resfs_orphan_{file_id}

EXT_OVERFLOW SEG 0:
  → collect all segments with this file_id
  → assemble by seg_index
  → file flagged as potentially inconsistent

Two valid SEG 0 with identical file_id, different generation:
  → take segment with higher generation (deterministic tiebreak)

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
| `mkfs.resfs`      | Format partition: write BH, WIA, SR, IR1/2/3, EOP    |
| `resfs-mount`     | Mount via platform VFS adapter (RhCOS, custom kernel) |
| `resfs-recover`   | Full disk scan → reconstruct all files                |
| `resfs-verify`    | Verify BLAKE3 integrity of all segments; --rebuild-bitmap |
| `resfs-visualize` | ASCII visualization of segment and free space layout  |
| `resfs-snap`      | Create, list, restore, delete snapshots               |
| `resfs-export`    | Extract raw file or recovery container from ResFS     |
| `resfs-import`    | Import from ext4/NTFS/exFAT/APFS to ResFS            |

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

| Platform            | Suitable  | Notes                                 |
| ------------------- | --------- | ------------------------------------- |
| Embedded / IoT      | ✅ Primary | Small disks, power loss tolerance     |
| Video recorders     | ✅         | Partial file recovery on power loss   |
| Routers / cameras   | ✅         | Configs, logs, small files            |
| Desktop             | ✅         | CoW makes it fully viable             |
| Server              | ✅         | CoW + snapshots                       |
| Linux               | ✅         | Via platform adapter or kernel module |
| macOS Intel         | ✅         | Via platform adapter                  |
| macOS Apple Silicon | ⚠️        | Requires disabling Secure Boot        |
| Windows             | ✅         | Via WinFSP                            |
| RhK                 | ✅ Target  | libresfs portable, no OS deps         |

---

## Minimum Requirements

- Minimum partition size: 64 MB
- Minimum block size: 4096 bytes (fixed)
- Maximum partition size: 64 ZiB (u64 LBA × 4KB blocks)
- Maximum files per partition: 2^64 (u64 file_id counter)
- Maximum file size: 183 extents × u64 length_blocks × 4KB

---

## Implementation Roadmap

### Phase 1 — libresfs core

**Criterion: create 10k files, kill -9, recover everything via full scan**

- [ ] `disk.img` creation: BH + WIA + SR + IR1/2/3 + EOP + Data Region
- [ ] Bootstrap Header: read / write / verify BLAKE3
- [ ] WIA Region: read / write / verify BLAKE3 / clear
- [ ] EOP: write / verify / use in recovery
- [ ] Segment: read / write / verify BLAKE3 (header + footer)
- [ ] SEG 0: IS_INLINE mode and extent mode
- [ ] CoW write path: IS_COMMITTED as atomic gate
- [ ] Root directory: file_id=1, "RESFS ROOT " signature
- [ ] SMI: on-disk format, read, write, rebuild from SEG 0 segments
- [ ] DLI: on-disk format, read, write, rebuild from directory segments
- [ ] IR copy selection: highest valid generation wins
- [ ] IR Expansion: sequential, one IR at a time, others remain active
- [ ] Free space bitmap: build from SMI + SR extents on mount
- [ ] Bitmap inline update: COW_WRITE, DELETE, TRUNCATE
- [ ] Mount: WIA-guided recovery + fallback full scan
- [ ] Recovery: full disk scan → rebuild SMI+DLI → mount
- [ ] `resfs-recover` proof of concept on disk.img
- [ ] Corruption test suite: random corruption, kill -9, partial writes

### Phase 2 — Advanced Features

- [ ] xattr full implementation
- [ ] ACL full implementation
- [ ] Snapshot management
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

- [ ] libresfs complete and tested
- [ ] `resfs-recover` demo: wipe GPT → full recovery
- [ ] Public `resfs` repo
- [ ] Submit GUID to gdisk partition type database
- [ ] Submit PR to linux/fs

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
Superblock removed. Journal removed — ResFS is pure CoW.
Bootstrap Header introduced. Disk layout corrected.
Each IR = 1 SMI + 1 DLI. BH carries fs_uuid, fs_label, IR locations.

### v1.2
file_id: counter+CSPRNG → plain u64 monotonic counter.
EOP added. IR regions fixed-size. Segment header reduced.
Free space model: in-memory bitmap only.

### v1.3
BH magic: "RESFS PARTITION " (16 bytes). SMI Entry: 64B → 28B.
Extent: 32B → 24B. DHT → DLI. DLI redesigned: sorted array + binary search.

### v1.4
IS_COMMITTED semantics redesigned: only on SEG 0, temporary flag.
FS_DIRTY flag added. CoW write path rewritten. TRUNCATE formalized.
Defragmenter introduced.

### v1.5
WIR (Write Intent Record) introduced. Disk layout updated.
BH and EOP updated. Dirty mount procedure formalized.
All magic constants unified. DLI collision resolution formalized.
GC + Snapshot interaction formalized. IR Expansion moved to Phase 1.

### v2.0
- WIR → WIA (Write Intent Array) throughout
- FS_DIRTY flag removed: mount always checks WIA entry_count
- WIA overflow: block new writes until IR flush and WIA clear (no overflow flag)
- WIA entry now stores complete extent list of file (not just new extents)
- SMI Entry redesigned: {file_id u64, seg0_lba u64} — 16 bytes
  Extent pool removed from SMI — extents live in SEG 0
- SEG 0 redesigned as file manifest:
  carries complete extent list, generation counter, IS_INLINE flag
  IS_INLINE=1: data inline in SEG 0 (≤ 3688B), no additional blocks
  IS_INLINE=0: up to 183 extents in SEG 0
  EXT_OVERFLOW flag for pathological fragmentation (>183 extents)
- generation counter on SEG 0 only (removed from all other segments)
- created_at u64 nanoseconds added to every segment (8B overhead)
- Snapshot model redesigned:
  SR entry stores {snapshot_id, snap_file_id, created_at, label, live}
  Each snapshot owns a snap file (IS_SNAPSHOT_FILE) in Data Region
  Snap file: flat array of {start_lba, length_blocks, seg_index, file_id, created_at}
  snapshot_id on superseded segment = newest live snapshot with created_at > segment.created_at
  SNAPSHOT_DELETE transfers blocks to previous live snapshot if created_at > block.created_at
- IS_SNAPSHOT_SEG removed: snapshot ownership determined by created_at comparison
- GC removed as separate process: bitmap updated inline on COW_WRITE, DELETE, TRUNCATE
- Bitmap construction at mount: SMI extents + all live snap file extents
- resfs-gc tool removed: verify --rebuild-bitmap for manual bitmap rebuild
- resfs-label tool removed
- resfs-export and resfs-visualize and resfs-import added to tooling
- version field split: u32 → {major u8, minor u8, patch u16} in BH and EOP
- version field removed from all other structures (SMI, DLI, WIA)
- DLI Header: entry_size field removed (constant 24B)
- Extent: seg_start_index and seg_count removed → {start_lba u64, length_blocks u64, seg_index u32} — 20 bytes
- Minimum partition size: 64 MB
- Full atomics write path formalized for all operations: CREATE, COW_WRITE, COW_DEFRAG, COW_EXPAND, TRUNCATE, DELETE, RENAME, SNAPSHOT_CREATE, SNAPSHOT_DELETE

---

*ResFS Specification v2.0*

*Author: Andrei Kovalenko*

*License: MIT License*