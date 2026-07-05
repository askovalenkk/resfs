<img width="1000" alt="ResFS Specification v2.0" src="https://github.com/user-attachments/assets/a4fb2c42-79d5-45f2-a739-597b0282f63e" />

> Recovery-First Filesystem — every physically intact segment is recoverable, deterministically, without heuristics, even if all metadata is destroyed.

## What ResFS is (and is not)

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
46534552-4C49-5345-5953-54454D2F414B
```

When interpreted using the standard GUID mixed-endian byte order, it decodes to "RESFILESYSTEM/AK" in ASCII — filesystem name and author initials.
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
WIA only records which file was being written, its operation type, and
where its manifest (SEG 0) landed — so that on recovery, the scanner knows
exactly which SEG 0 blocks to check for IS_COMMITTED, instead of scanning
the entire Data Region.

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
- **SMI header** carries allocation state (file_counter)
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
adjacent expansion buffers when SMI or DLI reaches 95% capacity.
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

BH exists as a single on-disk copy. It is not replicated. Its counterpart
EOP carries nearly identical layout fields and acts as its recovery backup
(see BH & EOP Mutual Backup).

```
Offset  Size    Field                    Description
------  ----    -----                    -----------
0       16      BH_SIG                   Magic: "RESFS PARTITION "
16      1       version_major            u8
17      1       version_minor            u8
18      2       version_patch            u16
20      4       block_size               u32, always 4096
24      8       total_blocks             u64, total number of 4096-byte blocks in the partition
32      16      fs_uuid                  UUID of this filesystem instance
48      1       label_len                u8, length of fs_label
49      255     fs_label                 UTF-8 filesystem label, null-padded
304     4       feature_flags            u32, see Feature Flags section
308     8       wia_start                u64, LBA of WIA Region (always 1)
316     8       wia_size                 u64, size of WIA Region in blocks
324     8       sr_start                 u64, LBA of Snapshot Region
332     8       sr_size                  u64, size of Snapshot Region in blocks
340     8       ir1_start                u64, LBA of IR1 (fixed at mkfs, never changes)
348     8       ir2_start                u64, LBA of IR2 (fixed at mkfs, never changes)
356     8       ir3_start                u64, LBA of IR3 (moves left on expansion)
364     8       ir_size                  u64, current size of each IR in blocks (grows on expansion)
372     8       data1_start              u64, LBA of Data Region 1
380     8       data2_start              u64, LBA of Data Region 2
388     8       start_of_partition       u64, Absolute LBA of the first logical sector
396     8       partition_size           u64, Partition size in logical sectors
404     4       logical_sector_size      u32, Logical sector size of the partition
408     32      blake3_hash              BLAKE3 of bytes [0..407]
440     3656    reserved                 Must be zero (pad to 4096 bytes)
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

ir_size        = max(MIN_IR_BLOCKS, total_blocks * 2 / 1000)  (~0.2% per IR copy)
MIN_IR_BLOCKS  = 128 (512KB per IR copy)

buffer_blocks      = max(MIN_BUFFER_BLOCKS, total_blocks * 3 / 1000)  (~0.3% per buffer)
MIN_BUFFER_BLOCKS  = 192 (768KB per buffer)

sr_size = max(RESFS_MIN_SR_BLOCKS, total_blocks * 2 / 1000)  (~0.2% of partition)
MIN_SR_BLOCKS = 8 (32KB)

wia_start   = 1
sr_start    = wia_start + wia_size
ir1_start   = sr_start + sr_size
data1_start = ir1_start + ir_size + buffer_blocks
ir2_start   = total_blocks / 2
data2_start = ir2_start + ir_size + buffer_blocks
ir3_start   = total_blocks - ir_size - 1
EOP: last_lba = total_blocks - 1
```

### SMI/DLI Block Distribution (per IR copy)

For a given IR copy of `IR_BLOCKS` blocks:

```
smi_body_blocks = floor((IR_BLOCKS - 2) / 5 * 2)
dli_body_blocks = ceil((IR_BLOCKS - 2) / 5 * 3)

[SMI Header:    1 block ]
[SMI Body:      smi_body_blocks blocks]
[DLI Header:    1 block ]
[DLI Body:      dli_body_blocks blocks]
[zeroed padding to end of IR region]
```

Ratio reflects entry sizes: SMI entry = 16B, DLI entry = 24B (1.5× larger),
so DLI body receives 1.5× more blocks than SMI body.

At minimum (IR_BLOCKS = 128):
```
smi_body_blocks = floor(126 / 5 * 2) = floor(50.4) = 50
dli_body_blocks = ceil(126 / 5 * 3)  = ceil(75.6)  = 76
total: 1 + 50 + 1 + 76 = 128 blocks ✓
```

---

## End Of Partition (EOP)

EOP is a physical boundary marker. It occupies the last block of the
partition (`last_lba`). It is not part of any IR. It is not a cache.
It is not a source of truth about files.

EOP is used during recovery when GPT is destroyed and partition
boundaries are unknown, and as a recovery backup for BH (see
BH & EOP Mutual Backup). It is not required for normal operation.

```
Offset  Size    Field                   Description
------  ----    -----                   -----------
0       8       EOP_SIG                 Magic: "ResFSEOP"
8       1       version_major           u8
9       1       version_minor           u8
10      2       version_patch           u16
12      4       block_size              u32, always 4096
16      8       total_blocks            u64, total number of 4096-byte blocks in the partition
24      16      fs_uuid                 UUID of this filesystem instance
40      1       label_len               u8, length of fs_label
41      255     fs_label                UTF-8 filesystem label, null-padded
296     4       feature_flags           u32, see Feature Flags section
300     8       wia_start               u64, LBA of WIA Region (always 1)
308     8       wia_size                u64, size of WIA Region in blocks
316     8       sr_start                u64, LBA of Snapshot Region
324     8       sr_size                 u64, size of Snapshot Region in blocks
332     8       ir1_start               u64, LBA of IR1
340     8       ir2_start               u64, LBA of IR2
348     8       ir3_start               u64, LBA of IR3
356     8       ir_size                 u64, current size of each IR in blocks
364     8       data1_start             u64, LBA of Data Region 1
372     8       data2_start             u64, LBA of Data Region 2
380     8       start_of_partition      u64, Absolute LBA of the first logical sector
388     8       partition_size          u64, Partition size in logical sectors
396     4       logical_sector_size     u32, Logical sector size of the partition
400     32      blake3_hash             BLAKE3 of bytes [0..399]
432     3642    reserved                Must be zero
4074    22      EOP_TAIL                "END OF RESFS PARTITION"
```

On hex dump the last block ends visually as:
```
...00 00 45 4E 44 20 4F 46 ..END OF
   20 52 45 53 46 53 20 50 RESFS P
   41 52 54 49 54 49 4F 4E ARTITION
```

**EOP is 4096 bytes (one block), fixed.**

### BH & EOP Mutual Backup

BH and EOP are both single, non-replicated structures, but they carry
nearly identical layout fields (`wia_start`, `sr_start`, `ir1/ir2/ir3_start`, 
etc.) — the only field that changes during the filesystem's lifetime, 
`ir_size` / `ir3_start`, is present and kept current in both.

Each is protected by its own independent BLAKE3. If one is corrupted,
the other supplies the same information:

```
BH BLAKE3 invalid  → load layout fields from EOP instead
EOP BLAKE3 invalid → load layout fields from BH instead
Both invalid        → brute-force scan for "ResFSSMI" magic (see BH Lost or Corrupted)
```

This is why IR Expansion writes EOP before BH (see IR Expansion,
Expansion algorithm, step 2) — whichever of the two is written second is
the one that can fail without loss of information.

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

---

## WIA — Write Intent Array

The WIA Region is a fixed-size array of write intent entries, allocated
at mkfs. It lives at block 1 — immediately after BH — and is always
findable without any pointer.

WIA is a crash recovery accelerator. Before any CoW operation, a WIA entry
is written recording which file's SEG 0 is about to change and
why. On recovery, the scanner reads WIA and checks only those specific
SEG 0 blocks for IS_COMMITTED — it never needs to re-derive the extent
list, because SEG 0 is self-describing (see Why extents in SEG 0, not in WIA).

**WIA is not a journal.** No operations are replayed. No redo log.
If IS_COMMITTED is not set on SEG 0, the write is simply discarded —
the old version survives in SMI. WIA only tells the scanner where to look.

If WIA itself is corrupt or absent on recovery, the fallback is a full
Data Region scan. The result is always identical — WIA only affects speed.

**WIA overflow:** if the WIA region is full (no free slots for a new
entry), all new write operations block until an in-flight operation
completes and its slot is freed. This is expected to be extremely rare —
capacity scales with disk size, and every entry is fixed at 20 bytes
regardless of how large or fragmented the file it describes is.

### WIA Header (4096 bytes, fixed)

```
Offset  Size    Field           Description
------  ----    -----           -----------
0       8       WIA_SIG         Magic: "ResFSWIA"
8       4       reserved        u32, must be 0
12      8       entry_count     u64, number of active (non-free) entries
20      8       capacity        u64, maximum entries (computed from wia_size)
28      8       data_offset     u64, byte offset to entry array (relative to WIA start; always block 2)
36      32      blake3_hash     BLAKE3 of entire WIA body (header + entries)
68      4028    reserved        pad to 4096 bytes
```

WIA has a single, non-replicated on-disk copy — unlike IR, it carries no
`generation` field. There is nothing to arbitrate between: WIA has exactly
one instance, and its own BLAKE3 is the only integrity signal it needs.

### WIA Entry (20 bytes, fixed)

```
Offset  Size    Field       Description
------  ----    -----       -----------
0       8       file_id     u64, the directory or file whose SEG 0 is being (re)written
8       8       seg0_lba    u64, LBA of the (new) SEG 0 for file_id
16      4       operation   u32, see WIA Operations
```

Entries are read directly from SEG 0 for their extent list — a WIA entry
never carries extents itself. This keeps every entry exactly 20 bytes
regardless of file size, fragmentation, or EXT_OVERFLOW chaining.

### WIA Operations

WIA `operation` is a bitfield, not a plain enum, so that RENAME can compose
two roles (source / destination) on top of the base operation code.

```
Bit     Meaning
---     -------
0       WIA_FREE_ENTRY      free entry flag
1       WIA_OP_CREATE       new file creation
2       WIA_OP_WRITE        CoW rewrite (partial or full, includes TRUNCATE)
3       WIA_OP_DEFRAG       defragmenter CoW relocation
4       WIA_OP_EXPAND       IR expansion CoW relocation
5       WIA_OP_DELETE       soft delete (IS_DELETED gate)
6       WIA_OP_RENAME       base flag for a rename operation
7       WIA_RENAME_OF       rename destination (new parent gaining the entry)
8       WIA_RENAME_IF       rename source (old parent losing the entry)
9-31    Reserved, must be 0

A slot with `WIA_FREE_ENTRY == 0` is free and available to the allocator.
```

### WIA Entry Allocation

Entries live in fixed slots within the WIA Region, in **no particular
order on disk**. The allocator does not sort or shift entries; it simply
places a new entry into any free slot (`operation == 0`) and clears it
back to `operation == 0` on completion.

**Per-block padding.** A 4096-byte block holds `floor(4096 // 20) = 204`
entries (4080 bytes), leaving 16 bytes of unused space at the end of
every block. Entries never straddle a block boundary — the 205th entry
of a block's worth of data always starts at the beginning of the next
block, not partway through the previous one. This is what makes a
single WIA block write atomic for every entry inside it, not just for
adjacent RENAME pairs (see RENAME pair allocation below): the 4KB block
is the unit of atomic I/O, and no entry is ever split across two such
units.

The 16 trailing bytes of each block are reserved and always zero. They
are never interpreted as an entry — `operation == 0` at that offset does
not need to be checked as a "free slot" because the allocator's slot
numbering skips over these bytes entirely; they simply do not correspond
to any slot index.

```
[WIA data block]  (4096 bytes)
    [Entry 0]  20B
    [Entry 1]  20B
    ...
    [Entry 203] 20B
    [reserved]  16B, must be zero, not an addressable slot
```

**In-memory index.** WIA is not searched linearly at runtime. At mount,
libresfs performs one full pass over all `capacity` slots and builds:

- A **sharded hash map** `file_id → slot_index` (`slot_index` here means
  a logical slot number in `[0, capacity)`, mapped to its physical byte
  offset by `block = slot_index / 204`, `offset_in_block = (slot_index mod 204) * 20`
  — the 16-byte per-block reserved region is never assigned a slot_index),
  split into N shards (`shard_id = hash(file_id) mod N`), each shard
  guarded by its own lock. Lookup, insert, and remove for a given
  `file_id` only ever take that file_id's shard lock — operations on
  unrelated file_ids never block each other.
- A **free-list** of unoccupied slot indices, guarded by a single separate
  lock. Free-list operations are a single push/pop of a slot index, so
  contention on this lock is minimal even though it is not sharded.

This in-memory index is purely a runtime structure — it is rebuilt from
scratch at every mount and has no on-disk representation of its own.

**RENAME pair allocation.** A rename operation needs two entries — one
`WIA_RENAME_OF` and one `WIA_RENAME_IF` — placed in **two physically
adjacent slots** (slot N and slot N+1), because their pairing on recovery
is determined purely by adjacency, not by any linking field (see RENAME
write path). If two adjacent free slots are not currently available, the
rename operation waits until the allocator can free up an adjacent pair.
Both slots for a rename pair must additionally fall within the same
4096-byte WIA block, so that the pair is written by a single atomic block
write — a crash can never leave one half of the pair written without
the other. Because slot indices never span the per-block reserved bytes
(see Per-block padding above), any adjacent pair (N, N+1) with
`N mod 204 != 203` is guaranteed to already satisfy this same-block
requirement; the allocator only needs to skip pairing slot 203 of one
block with slot 0 of the next.

### WIA Write Path

A WIA entry (or entry pair, for RENAME) is written before any CoW
operation begins after new segments allocated:

```
1. Allocate new file segments in memory
2. Allocate a free slot (or adjacent pair, for RENAME)
3. Write WIA entry/entries (file_id, seg0_lba, operation)
4. Verify BLAKE3 of WIA
5. Proceed with the CoW operation
```

Crash after step 2: WIA entry is valid → recovery reads it → checks
the referenced SEG 0 for IS_COMMITTED.

### WIA on Recovery

```
1. Read WIA Header — verify BLAKE3
   if invalid → fallback to full Data Region scan
2. For each occupied WIA entry ((operation << 0) != 0):
   a. If operation has WIA_OP_RENAME → skip entirely here, handle
      exclusively via RENAME Recovery (steps b-d below do not apply —
      a RENAME entry's file_id identifies a directory, not the file
      being moved, and its recovery semantics are pairwise, not a plain
      SMI add/remove)
   b. Read SEG 0 at seg0_lba
   c. Check SEG 0 for IS_COMMITTED
   d. IS_COMMITTED=1 → write was committed, IR not yet updated
      → read extents directly from this SEG 0
      → add/update {file_id, seg0_lba} in SMI
      → if operation has WIA_OP_DELETE → SEG 0 also has IS_DELETED set
        → remove {file_id} from SMI and DLI instead of adding
   e. IS_COMMITTED=0 → write was aborted
      → mark new blocks free in bitmap (blocks listed in this SEG 0's
        extents, not yet referenced by any committed SMI entry)
      → if operation has WIA_OP_DEFRAG → file remains in defrag queue
3. Process all WIA_OP_RENAME entries — see RENAME Recovery
4. Update all three IR copies with recovered SMI/DLI state
5. Clear WIA (zero entry_count, clear every recovered slot to
   operation == 0, recompute BLAKE3)
```

### RENAME Recovery

```
1. Scan WIA for entries carrying WIA_OP_RENAME.
2. For each WIA_RENAME_OF entry found at slot N:
   a. slot N+1 must carry WIA_RENAME_IF for the same rename — this is
      guaranteed by allocation (see WIA Entry Allocation), not by any
      stored link.
   b. Check IS_COMMITTED on the OF entry's SEG 0 (destination directory):
      - If IS_COMMITTED=1 → destination directory already has the new
        entry. Proceed to check the IF entry.
      - If IS_COMMITTED=0 → destination write never landed. Discard
        both entries; the rename never took effect. Filesystem is
        consistent as if RENAME was never called.
   c. If OF is committed, check IS_COMMITTED on the IF entry's SEG 0
      (source directory):
      - If IS_COMMITTED=1 → both halves committed. Normal completion:
        update SMI/DLI for both directories.
      - If IS_COMMITTED=0 → destination already has the entry, but the
        source directory has not yet been rewritten to remove it. The
        file is temporarily visible under both names. Recovery completes
        the pending half: apply the source directory's removal exactly
        as the original operation intended.
3. Clear both slots to operation == 0.
```

This makes the intermediate state (visible in both directories) a safe
superset, not a hole — RENAME never has a window in which the file is
invisible from both paths (see RENAME write path for why the OF write
always precedes the IF write).

### WIA Capacity

```
entries_per_block = floor(4096 / 20) = 204
data_blocks       = wia_size - 1        (block 0 is the WIA Header)
capacity          = data_blocks * entries_per_block
```

On a 1TB disk (wia_size ≈ 256 blocks, 1GB): capacity ≈ 256 * 204 ≈ 52,000 entries.
On a 64MB embedded disk (wia_size = 16 blocks, 64KB): capacity ≈ 15 * 204 ≈ 3,060 entries.

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
24      4       live                u32, 1 = live, 0 = deleted
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
16      4       ext_index           u32, read order of this extent within the file
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

The SMI header carries `file_counter` — allocation state that belongs
to the physical layer.

### SMI Header (4096 bytes, fixed)

```
Offset  Size    Field               Description
------  ----    -----               -----------
0       8       SMI_SIG             "ResFSSMI"
8       8       reserved            u64, must be 0
16      8       generation          u64, incremented on each IR write
24      8       file_counter        u64, next file_id to assign
32      8       last_mount          u64, Unix timestamp nanoseconds
40      8       entry_count         u64
48      32      blake3_hash         BLAKE3 of entire SMI body
80      4016    reserved            pad to 4096 bytes
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
10. Build free space bitmap from SMI + SR extents (see Bitmap Construction)
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

### DLI On-Disk Packing

A 4096-byte block holds `floor(4096 / 24) = 170` entries (4080 bytes),
leaving 16 reserved bytes at the end of every block — the same situation
as WIA entries (see WIA Entry Allocation, Per-block padding), and for the
same reason: entries must not straddle a block boundary, so that a single
block write is always atomic for every entry inside it.

This means the binary search in Lookup Algorithm below cannot address an
entry by a flat `mid * 24` byte offset — it must convert the logical
entry index to a physical byte offset that accounts for the per-block
gap:

```
block      = mid / 170
offset     = (mid % 170) * 24
byte_offset = block * 4096 + offset
```

Because DLI is always rebuilt as a whole (see DLI Rebuild) rather than
updated entry-by-entry in place, this padding costs nothing beyond the
same ~0.4% overhead as WIA — the rebuild simply stops packing entries
16 bytes before the end of each block instead of packing them
edge-to-edge across the whole array.

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
    entry = read_dli_entry(mid)   // see DLI On-Disk Packing for the
                                   // logical-index → byte_offset conversion

    if entry < key:
        low = mid + 1
    else if entry > key:
        high = mid - 1
    else:
        candidate_file_id = entry.file_id
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

IR expansion is triggered when **either** SMI **or** DLI within any IR copy
reaches 95% of its allocated block capacity. A single table filling up is
sufficient to trigger expansion — both do not need to be full simultaneously.

IRs expand when triggered. Expansion is sequential —
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
    EOP reflects the intended new state — see BH & EOP Mutual Backup)

3. For each file in expansion buffer zone:
   a. COW_WRITE (WIA_OP_EXPAND): relocate file to free Data Region space
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

Each individual relocation in step 3 is already crash-safe on its own
(it is an ordinary WIA-guarded COW_WRITE) — a crash partway through step 3
simply leaves some files relocated and others not, which the next
IR Expansion attempt naturally continues.

**Capacity on 1TB disk:**

| ir_size     | files supported |
| ----------- | --------------- |
| 0.2% (~2GB) | ~55M files      |
| 0.5% (~5GB) | ~137M files     |

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
13      IS_POINTER_SEG      overflow extent continuation block (see EXT_OVERFLOW); carries extents beyond SEG 0 capacity
14      IS_SNAPSHOT_FILE    this file is a snapshot extent list (owned by SR)
15      EXT_OVERFLOW        SEG 0 only: extent count exceeds 183, ptr_lba points to IS_POINTER_SEG
16-31   Reserved, must be 0
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

When a file grows beyond 3680 bytes, it is rewritten via COW_WRITE:
new SEG 0 without IS_INLINE + data in separate segments.

### IS_INLINE = 0 (regular files)

```
[Segment header: 88B]
[SEG 0 header: 304B]
[reserved: 4B]
[ptr_lba: u64, 8B]
[extent_count: u64, 8B]
[extents: extent_count × 20B]
[zeroed padding to footer]
[Segment footer: 24B]
```

Maximum extents: (4096 - 88 - 304 - 4 - 8 - 8 - 24) / 20 = 3660 / 20 = **183 extents**.

`ptr_lba` is valid only when `EXT_OVERFLOW` is set. It contains the LBA of the
first `IS_POINTER_SEG` block carrying the continuation of the extent list.
When `EXT_OVERFLOW` is not set, `ptr_lba` is zero and ignored.

### EXT_OVERFLOW and IS_POINTER_SEG

If a file accumulates more than 183 extents (pathological fragmentation —
the defragmenter threshold is 8 extents, so this is practically impossible
under normal operation), the following applies:

```
1. New writes are blocked for this file
2. Defragmenter is forced to run immediately
3. If defragmentation cannot reduce extent count to ≤ 183:
   a. EXT_OVERFLOW is set on SEG 0
   b. ptr_lba is set to the LBA of a new IS_POINTER_SEG block
   c. IS_POINTER_SEG carries the continuation of the extent list
      using the same resfs_seg0 structure with IS_POINTER_SEG flag
   d. IS_POINTER_SEG copies all metadata from SEG 0 (file_id, file_size,
      created_at, permissions, etc.), filename_len = 0
   e. If IS_POINTER_SEG itself overflows: set EXT_OVERFLOW on it,
      ptr_lba points to the next IS_POINTER_SEG (chained)
```

On full disk scan, a file with `EXT_OVERFLOW` is recovered by following the
`ptr_lba` chain. Recovery remains deterministic as long as all blocks in the
chain are physically intact. The file is flagged as potentially inconsistent
in `recovery_info`.

---

## Extent (20 bytes, fixed)

```
Offset  Size    Field               Description
------  ----    -----               -----------
0       8       start_lba           u64
8       8       length_blocks       u64
16      4       ext_index           u32, read order of this extent within the file
```

Extents describe contiguous runs of segments on disk. `ext_index` identifies
the read order of this extent, allowing the reader to assemble the file in
correct order regardless of physical layout on disk.

---

## File ID

```c
uint64_t file_id;
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
flags          = IS_FIRST_SEG | IS_DIRECTORY
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
[1 byte: name_len][255 bytes: name][8 bytes: file_id][4 bytes: flags]
```

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
link1: file_id=3, hardlink_id=5, name="photo.jpg"   ← own SEG 0
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
392     4       major_number    u32
396     4       minor_number    u32
404     8       driver_id       u64
408     4       device_flags    u32
412     4       device_type     u32
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
2. Check WIA in-memory index: if entry with this file_id exists → wait/error
3. Allocate blocks in bitmap (in memory)
4. Allocate a free WIA slot; write WIA entry (file_id, seg0_lba, WIA_OP_CREATE)
5. Write all segments with created_at = now()
6. Compute BLAKE3, verify
7. Set IS_COMMITTED on SEG 0
8. Update SMI: add {file_id, seg0_lba}
9. Update DLI: add {name_hash, file_id, parent_dir_id}
10. Write IR1 → IR2 → IR3
11. Clear IS_COMMITTED on SEG 0
12. Free the WIA slot (WIA_FREE_ENTRY → 0)
```

### COW_WRITE

```
1. Check WIA in-memory index: if entry with this file_id exists → wait/error
2. Allocate new blocks in bitmap
3. Allocate a free WIA slot; write WIA entry (file_id, new seg0_lba, WIA_OP_WRITE)
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
12. Free the WIA slot (WIA_FREE_ENTRY → 0)
```

### COW_DEFRAG

```
1. Check WIA in-memory index: if entry with this file_id exists → wait/error
2. Allocate new contiguous blocks in bitmap
3. Allocate a free WIA slot; write WIA entry (file_id, new seg0_lba, WIA_OP_DEFRAG)
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
12. Free the WIA slot (WIA_FREE_ENTRY → 0)
```

### IR Expansion relocation

IR Expansion relocates files out of the expansion buffer zone using COW_WRITE (see IR Expansion) 
tagged with `WIA_OP_EXPAND` instead
of `WIA_OP_WRITE`, purely so that recovery can distinguish "this write was
part of an expansion" from an ordinary user write when inspecting WIA.
The write path itself is otherwise identical to COW_WRITE above.

### TRUNCATE

```
1. Check WIA in-memory index: if entry with this file_id exists → wait/error
2. Find SEG 0 via SMI (seg0_lba)
3. Read extents from SEG 0
4. Determine segments beyond new_size
5. Allocate new block for new SEG 0 in bitmap
6. Allocate a free WIA slot; write WIA entry (file_id, new seg0_lba, WIA_OP_WRITE)
7. Write new SEG 0 with generation+1, trimmed extents, updated file_size
8. Compute BLAKE3, verify
9. Set IS_COMMITTED on new SEG 0
10. Update SMI: seg0_lba → new address
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
14. Free the WIA slot (operation → 0)
```

TRUNCATE reuses `WIA_OP_WRITE` rather than a dedicated code — from WIA's
perspective it is just another CoW rewrite of SEG 0; the distinction is
not needed on recovery.

### DELETE

DELETE is a CoW mutation of SEG 0 (setting IS_DELETED) followed by
metadata removal, so it is WIA-guarded exactly like the other operations —
a crash between setting IS_DELETED and updating SMI/DLI must not leave the
filesystem believing a deleted file is still live.

```
1. Check WIA in-memory index: if entry with this file_id exists → wait/error
2. Find SEG 0 via SMI (seg0_lba)
3. Allocate a free WIA slot; write WIA entry (file_id, seg0_lba, WIA_OP_DELETE)
4. If sr.live_count == 0:
     Clear all file blocks in bitmap
   If sr.live_count > 0:
     For each segment of this file:
       If newest_live_snapshot.created_at > segment.created_at:
         Set segment.snapshot_id = newest_live_snapshot.snapshot_id
         Append extent to that snapshot's snap file
       Else:
         Clear block in bitmap
     Set IS_DELETED on SEG 0
5. Remove from DLI
6. Remove from SMI
7. Write IR1 → IR2 → IR3
8. Free the WIA slot (operation → 0)
```

Recovery: if a WIA_OP_DELETE entry is found and its SEG 0 has IS_DELETED
set, but SMI/DLI still reference the file, recovery removes those SMI/DLI
entries — completing the delete exactly as WIA on Recovery step 2c
describes. If IS_DELETED is not set, the delete never took effect and
SMI/DLI are left untouched.

### RENAME

RENAME moves a directory entry from one parent directory to another (or
renames it within the same parent). Each parent directory is itself a
file with its own SEG 0 — moving an entry between two different parents
is therefore two independent CoW rewrites (of the source and destination
directory files), not one. To avoid a window where the entry exists in
neither directory, the destination is always written and committed
**before** the source is rewritten to remove it.

```
Same-parent rename (old parent == new parent):

1. Find file_id via DLI
2. COW_WRITE the parent directory segment: remove old entry, add new
   entry with the new name, in a single new SEG 0
3. Update DLI entry in-place: new name_hash
4. Write IR1 → IR2 → IR3
```

This case is a single ordinary COW_WRITE (see COW_WRITE above) — no WIA
pairing is needed because only one directory's SEG 0 changes.

```
Cross-parent rename (old parent != new parent):

1. Find file_id via DLI
2. Allocate new blocks in bitmap for the new parent directory's new SEG 0
   and for the old parent directory's new SEG 0 (both are ordinary CoW
   rewrites of a directory file, same as any COW_WRITE)
3. Allocate an adjacent WIA slot pair (see WIA Entry Allocation), now that
   both new SEG 0 LBAs are known from step 2:
   slot N   = {new_parent.file_id, new_parent's new seg0_lba, WIA_OP_RENAME | WIA_RENAME_OF}
   slot N+1 = {old_parent.file_id, old_parent's new seg0_lba, WIA_OP_RENAME | WIA_RENAME_IF}
4. Write new parent directory segment at its allocated SEG 0 LBA: add new
   entry with the new name
5. Set IS_COMMITTED on new parent's new SEG 0
6. Update SMI for new parent: seg0_lba → new address
7. Update DLI: add {name_hash, file_id, new_parent_dir_id}
8. Write IR1 → IR2 → IR3
9. Clear IS_COMMITTED on new parent's SEG 0
10. Write old parent directory segment at its allocated SEG 0 LBA: remove
    old entry
11. Set IS_COMMITTED on old parent's new SEG 0
12. Update SMI for old parent: seg0_lba → new address
13. Remove DLI entry: {name_hash, file_id, old_parent_dir_id}
14. Write IR1 → IR2 → IR3
15. Clear IS_COMMITTED on old parent's SEG 0
16. Free both WIA slots (operation → 0)
```

Between step 5 and step 11, the file is visible under its new name in the
new parent AND still visible under its old name in the old parent. This
is an intentional, safe intermediate state — not a hole. See RENAME
Recovery for how a crash in this window is resolved deterministically
from the WIA pair alone (no linking field needed — pairing is by slot
adjacency, see WIA Entry Allocation).

### SNAPSHOT_CREATE

```
1. snapshot_id = atomic_increment(sr.snapshot_counter)
2. Create snap file via CREATE (IS_SNAPSHOT_FILE flag, empty)
3. Write SR entry: {snapshot_id, snap_file_id, created_at=now(), live=1}
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

If `prev_snap` does not exist (the snapshot being deleted is the oldest
live one), every block in its snap file simply clears in the bitmap —
there is no older snapshot for those blocks to be transferred to.

---

## Free Space

In-memory bitmap of occupied blocks, rebuilt at every mount.
Not stored on disk — always reconstructable. There is no on-disk
block-usage counter anywhere in the filesystem (see SMI Header) — the
bitmap is the single source of truth for free space, both for allocation
and for any usage reporting.

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

8. Process WIA entries — see WIA on Recovery and RENAME Recovery

9. Update all three IR copies with recovered SMI/DLI state
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

See BH & EOP Mutual Backup for why this fallback is always available.

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
   d. If EXT_OVERFLOW → follow ptr_lba chain to collect all IS_POINTER_SEG blocks
6. If IS_POINTER_SEG:
   a. Record (file_id, lba) for association after scan
   b. If EXT_OVERFLOW → chain continues, follow ptr_lba
7. If not IS_FIRST_SEG and not IS_POINTER_SEG:
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
  → follow ptr_lba chain to find all IS_POINTER_SEG blocks for this file_id
  → assemble complete extent list from SEG 0 + pointer chain in order
  → file flagged as potentially inconsistent in recovery_info
  → if any IS_POINTER_SEG in chain is missing: remaining extents unrecoverable,
     gap filled with zeros, file flagged as partially recovered

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
| `mkfs.resfs`      | Format partition: write BH, WIA, SR, IR1/2/3, EOP     |
| `resfs-recover`   | Full disk scan → reconstruct all files                |
| `resfs-verify`    | Verify BLAKE3 integrity of all segments               |
| `resfs-snap`      | Create, list, restore, delete snapshots               |
| `resfs-export`    | Extract raw file or recovery container from ResFS     |
| `resfs-import`    | Import from ext4/NTFS/exFAT/APFS to ResFS             |
| `resfs-dump`      | Print human-readable FS layout                        |
| `resfs-info`      | Show and change FS info (label, feature flags, etc.)  |
| `resfs-db`        | FS debug interactive shell                            |
| `resfs-stat`      | FS statictics (fragmentation level, free space, etc.) |

---

## Build Model

libresfs core (libresfs/include/, libresfs/src/):
  - Freestanding — no libc dependency
  - Only stdint.h and stddef.h
  - Compiles on any platform including bare-metal kernels
  - Target: RhK kernel, any OS kernel

tools/ (mkfs, verify, recover, snap, export, import, visualize):
  - Requires POSIX-compatible libc and syscalls
  - Not freestanding
  - Target: Linux, RhCOS userspace, any Unix-like OS

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

## Platform Abstraction

libresfs is freestanding and has no dependency on any host OS.
All disk I/O is performed through a platform abstraction layer.

### resfs_platform

```c
struct resfs_platform {
    int (*read_block)(uint64_t lba, void *buf, void *ctx);
    int (*write_block)(uint64_t lba, const void *buf, void *ctx);
    void *ctx;
};
```

`read_block` and `write_block` must read/write exactly one 4096-byte
block at the given LBA. Return 0 on success, negative error code on failure.

`ctx` is an opaque pointer passed back to every call — use it to carry
platform-specific state (file descriptor, device handle, etc.).

### WIA In-Memory Concurrency Model

The on-disk WIA format is deliberately simple (fixed 20-byte slots, no
ordering requirement — see WIA Entry Allocation). All concurrency control
lives in libresfs's in-memory runtime layer, not in the on-disk format,
so it can evolve independently of the disk layout:

```c
struct resfs_wia_runtime {
    /* N shards, each an independent file_id -> slot_index map with its
       own lock. shard_id = hash(file_id) mod N. Operations on file_ids
       hashing to different shards never block each other. */
    struct resfs_wia_shard shards[RESFS_WIA_SHARD_COUNT];

    /* Single free-list of unoccupied slot indices, one separate lock.
       Not sharded: push/pop is O(1) and held only briefly. */
    struct resfs_wia_freelist freelist;
};
```

Rebuilt from scratch by a single full pass over the WIA Region at every
mount; never persisted.

### Known implementations

| Platform          | Location              |
|-------------------|-----------------------|
| RhK kernel        | RhK repository        |
| Linux kernel module | linux/ (future)     |

Tools in `tools/` do not use `resfs_platform` — they access disk images and
disks directly via POSIX `pread`/`pwrite`.

---

## Requirements and Limitations

- Minimum partition size: 16 MB
- Maximum partition size: 64 ZiB (u64 LBA × 4KB blocks)
- Maximum files per partition: 2^64 (u64 file_id counter)
- Maximum file size: 183 extents × u64 length_blocks × 4KB
- Maximum files (IR bottleneck): ~13.7M files per 100GB

---

## Implementation Roadmap

### Phase 1 — libresfs core

**Criterion: create 10k files, kill -9, recover everything via full scan**

- [x] `disk.img` creation: BH + WIA + SR + IR1/2/3 + EOP + Data Region
- [ ] Bootstrap Header: read / write / verify BLAKE3
- [ ] WIA Region: read / write / verify BLAKE3 / clear
- [ ] WIA in-memory index: sharded map + free-list, built at mount
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
- [ ] RENAME: adjacent WIA slot pair allocation + recovery
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
- WIA entry redesigned as fixed 20 bytes: {file_id, seg0_lba, operation} —
  no extents, no variable length, no heap/allocator inside WIA
- WIA overflow: new writes block until a slot frees up (fixed-size slots,
  no separate overflow flag needed)
- WIA `generation` field removed — WIA has a single non-replicated copy,
  nothing to arbitrate between
- WIA operations redefined as bitflags; WIA_OP_DELETE and WIA_OP_RENAME
  (with WIA_RENAME_OF / WIA_RENAME_IF) added
- WIA entries unsorted on disk; runtime uses a sharded in-memory
  file_id → slot index map plus a single free-list for allocation
- DELETE now goes through WIA like every other mutating operation
- RENAME across directories now WIA-guarded via an adjacent slot pair
  (OF/IF), pairing determined by physical slot adjacency, not a stored
  link; destination is always committed before source removal so the
  intermediate crash state is a safe superset (visible in both
  directories), never a hole
- SMI Entry redesigned: {file_id u64, seg0_lba u64} — 16 bytes
  Extent pool removed from SMI — extents live in SEG 0
- SMI Header: `used_blocks` field removed — bitmap is the sole source
  of truth for free space, never persisted or cached on disk
- COW_EXPAND folded into IR Expansion as ordinary WIA_OP_EXPAND-tagged
  COW_WRITE relocation — no separate defragmentation step
- SEG 0 redesigned as file manifest:
  carries complete extent list, generation counter, IS_INLINE flag
  IS_INLINE=1: data inline in SEG 0 (≤ 3680B), no additional blocks
  IS_INLINE=0: up to 183 extents in SEG 0
  EXT_OVERFLOW flag + ptr_lba for pathological fragmentation (>183 extents)
- ptr_lba field added to SEG 0 (replaces 8B of reserved3): u64 LBA of IS_POINTER_SEG
- IS_POINTER_SEG flag added (bit 13): overflow extent continuation block
  IS_SNAPSHOT_FILE moved to bit 14, EXT_OVERFLOW to bit 15
- IS_POINTER_SEG uses resfs_seg0 structure, copies metadata from SEG 0,
  carries continuation extents; chains via EXT_OVERFLOW + ptr_lba if needed
- seg_index renamed to ext_index in resfs_extent and resfs_snap_extent:
  ext_index is read order of the extent within the file, independent of seg_index
- generation counter on SEG 0 only (removed from all other segments)
- created_at u64 nanoseconds added to every segment (8B overhead)
- Snapshot model redesigned:
  SR entry stores {snapshot_id, snap_file_id, created_at, label, live}
  Each snapshot owns a snap file (IS_SNAPSHOT_FILE) in Data Region
  Snap file: flat array of {start_lba, length_blocks, ext_index, file_id, created_at}
  snapshot_id on superseded segment = newest live snapshot with created_at > segment.created_at
  SNAPSHOT_DELETE transfers blocks to previous live snapshot if created_at > block.created_at
- IS_SNAPSHOT_SEG removed: snapshot ownership determined by created_at comparison
- GC removed as separate process: bitmap updated inline on COW_WRITE, DELETE, TRUNCATE
- Bitmap construction at mount: SMI extents + all live snap file extents
- resfs-gc tool removed
- resfs-verify no longer exposes a bitmap-rebuild mode — the bitmap has
  no on-disk form to rebuild; it is only ever built fresh at mount
- resfs-label tool removed
- resfs-export and resfs-visualize and resfs-import added to tooling
- version field split: u32 → {major u8, minor u8, patch u16} in BH and EOP
- version field removed from all other structures (SMI, DLI, WIA)
- DLI Header: entry_size field removed (constant 24B)
- Extent: seg_start_index and seg_count removed → {start_lba u64, length_blocks u64, ext_index u32} — 20 bytes
- Minimum partition size: 16 MB
- Full atomics write path formalized for all operations: CREATE, COW_WRITE, COW_DEFRAG, IR Expansion relocation, TRUNCATE, DELETE, RENAME, SNAPSHOT_CREATE, SNAPSHOT_DELETE
- BH & EOP formalized as mutual recovery backups for each other's layout fields
- RENAME write path corrected: bitmap allocation for both directories'
  new SEG 0 blocks now explicitly precedes WIA pair allocation, since the
  WIA entry must record a real, already-allocated seg0_lba
- WIA on Recovery: WIA_OP_RENAME entries are now explicitly excluded from
  the generic SMI add/remove step and handled exclusively by RENAME
  Recovery, avoiding double-processing of the same entry under two
  incompatible interpretations

---

*ResFS Specification v2.0*

*Author: Andrei Kovalenko*

*License: MIT License*
