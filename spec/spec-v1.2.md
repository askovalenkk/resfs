# ResFS Specification v1.2
> Recovery-First Filesystem — every physically intact segment is recoverable,
> deterministically, without heuristics, even if all metadata is destroyed.

---

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
    DHT (Directory Hash Table)  ← namespace layer cache

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
| DHT says file exists, no segments found for file_id | File does not exist. DHT entry purged. |
| Two segments with identical file_id + seg_index, both valid BLAKE3 | Take segment at lower LBA (deterministic). |
| Segment missing in middle of chain (gap) | File partially recovered, gap filled with zeros, recovery_info written. |

---

## Official GUID

ResFS partition type GUID for GPT:

```
52455346-494C-4553-5953-54454D414B21
```

Encodes "RESFILESYSTEM" in ASCII.
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

If BH is destroyed, IR locations can be found by scanning for "SMI0" magic.
If all IRs are destroyed, segments reconstruct everything.
Nothing is irreplaceable.

---

## Disk Layout

```
[ResFS Partition]
  Block 0               Bootstrap Header (BH)
  Block 1..IR1_end      Index Region 1 (IR1) — fixed size, allocated at mkfs
  Block IR1_end+1..     Snapshot Region — fixed size, allocated at mkfs
  Block snap_end+1..    Data Region (segments)
  [partition midpoint]  Index Region 2 (IR2) — fixed size, allocated at mkfs
  [continued]           Data Region (continued)
  [near end]            Index Region 3 (IR3) — fixed size, allocated at mkfs
  [last block]          End Of Partition marker (EOP)
```

**Index Regions are fixed-size**, allocated at mkfs. They never grow into
adjacent regions. Unused space within an IR region is zeroed. As the SMI
and DHT grow, they expand into the pre-allocated zeroed space.

IR3 ends at `last_lba - 1` at maximum. EOP always occupies `last_lba`.

Three Index Regions distributed across the partition:
- **IR1**: immediately after BH (start of partition)
- **IR2**: at partition midpoint
- **IR3**: near partition end (right before EOP)

Each IR = 1 SMI + 1 DHT. Three IRs = three independent copies of SMI+DHT,
physically separated. BH carries the exact LBA of all three IRs.

---

## Bootstrap Header (BH)

Written once at `mkfs`. **Never updated during normal operation.**

The BH carries only immutable identity and structural pointers.
It is not a superblock. It has no counters, no mount timestamps,
no runtime state of any kind.

```
Offset  Size    Field           Description
------  ----    -----           -----------
0       8       BH_SIG          Magic: "ResFSBH0"
8       4       version         u32, format version (1)
12      4       block_size      u32, always 4096
16      16      fs_uuid         UUID of this filesystem instance
32      1       label_len       u8, length of fs_label
33      255     fs_label        UTF-8 filesystem label, null-padded
288     4       feature_flags   u32, see Feature Flags section
292     8       ir1_start       u64, LBA of IR1
300     8       ir2_start       u64, LBA of IR2
308     8       ir3_start       u64, LBA of IR3
316     8       ir_size         u64, size of each IR region in blocks (all equal)
324     8       snap_start      u64, LBA of Snapshot Region
332     8       snap_size       u64, size of Snapshot Region in blocks
340     8       data_start      u64, LBA of Data Region start
348     8       total_blocks    u64, total blocks in partition
356     32      blake3_hash     BLAKE3 of bytes [0..355]
388     124     reserved        Must be zero (pad to 512 bytes)
```

**BH is 512 bytes, fixed.**

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
0       8       EOP_SIG         Magic: "ResFsEOP"
8       4       version         u32 = 1
12      4       block_size      u32 = 4096
16      16      fs_uuid         UUID of this partition (matches BH)
32      8       part_start_lba  u64, LBA of partition start
40      8       part_end_lba    u64, LBA of partition end (this block)
48      8       data_start_lba  u64, LBA of Data Region start
56      8       ir1_lba         u64, LBA of IR1 (hint only)
64      8       ir2_lba         u64, LBA of IR2 (hint only)
72      8       ir3_lba         u64, LBA of IR3 (hint only)
80      32      blake3_hash     BLAKE3 of bytes [0..79]
112     3952    reserved        Must be 0x00
4064    32      EOP_TAIL        \x00\x00..."END OF RESFS PARTITION"
```

EOP_TAIL layout (32 bytes):
```
[10 bytes: 0x00] ["END OF RESFS PARTITION"]
```

On hex dump the last block ends visually as:
```
...00 00 00 45 4E 44 20 4F  ...END O
   46 20 52 45 53 46 53 20  F RESFS 
   50 41 52 54 49 54 49 4F  PARTITIO
   4E                       N
```

**EOP is 4096 bytes (one block), fixed.**

### EOP Usage During Recovery

```
1. GPT intact → partition boundaries known → EOP not needed

2. GPT destroyed → scan last N blocks of each device:
   if block[0..7] == "ResFsEOP":
     verify BLAKE3([0..79])
     if valid and fs_uuid matches → EOP found

3. EOP found:
   part_start_lba → partition start
   part_end_lba   → partition end
   ir1/ir2/ir3_lba → hints to locate IR copies

4. Attempt to load BH from part_start_lba:
   if BH valid → use IR locations from BH (authoritative)
   if BH invalid → use IR hints from EOP

5. Attempt to load IR copies:
   if valid IR found → mount with SMI+DHT
   if no valid IR → full segment scan from data_start_lba to part_end_lba

6. Full segment scan → rebuild SMI+DHT from segments
   segments are the only truth
```

### EOP Conflict Resolution

```
EOP absent or not written:
  → system operates normally, EOP is optional
  → recovery without GPT falls back to brute-force IR scan

EOP BLAKE3 invalid:
  → EOP ignored entirely
  → scanner searches for IR by "SMI0" magic brute-force

EOP fs_uuid does not match BH:
  → EOP ignored (foreign partition or corruption)
  → BH always wins on uuid conflict

EOP ir_lba hints conflict with BH:
  → BH wins (BH is authoritative, EOP hints are advisory)
  → EOP hints used only when BH is unavailable
```

### EOP Invariants

| Invariant | Status |
|-----------|--------|
| segments = only truth about files | ✅ EOP contains no file data |
| IR = cache only | ✅ EOP is not an IR, contains no SMI/DHT |
| no metadata outranks segments | ✅ EOP contains only physical boundaries |
| EOP is not required | ✅ FS operates fully without EOP |

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

**IS_COMMITTED is the CoW commit gate.**

Writing a new version of a file:
```
1. Write new segment(s) to free block(s) — IS_COMMITTED not set
2. fsync
3. Set IS_COMMITTED on new segment(s)     ← atomic commit point
4. Clear IS_COMMITTED on old segment(s)
   (or set IS_SNAPSHOT_SEG if snapshotting)
```

Power loss before step 3: new segments are invisible. Old version survives.
Power loss after step 3: new version is live. Old cleaned up by GC.
No journal. No replay. Deterministic.

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
328     8       created_at      u64, Unix timestamp (seconds)
336     8       modified_at     u64, Unix timestamp (seconds)
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
In memory: incremented atomically on every CREATE.
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
data[0..10]    = "RESFS_ROOT\0"   (signature at start of data)
```

The root directory signature `"RESFS_ROOT\0"` precedes directory entries
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
receives `IS_SNAPSHOT_SEG` and a `snapshot_id` reference — it is retained
and excluded from normal GC until the snapshot is deleted.

Snapshot metadata lives in the Snapshot Region (fixed, after IR1):

```
Offset  Size    Field
------  ----    -----
0       8       SNAP_SIG        "ResFSSNP"
8       8       snapshot_id     u64
16      8       created_at      u64, Unix timestamp
24      255     label           UTF-8 human-readable name
279     8       root_dir_id     file_id of root directory at snapshot time
287     1       is_deleted      0 = live, 1 = pending GC
288     32      blake3_hash     BLAKE3 of bytes [0..287]
```

### Snapshot GC

**Phase 1 — Mark (instant):** Set `is_deleted = 1`. Snapshot immediately
invisible to the filesystem. No segments touched.

**Phase 2 — Sweep (lazy, background):** `resfs-gc` scans all
`IS_SNAPSHOT_SEG` segments, checks `snapshot_id` liveness, frees blocks
of deleted snapshots.

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

### SMI Header (512 bytes, fixed)

```
Offset  Size    Field               Description
------  ----    -----               -----------
0       8       magic               "SMI0\0\0\0\0"
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
112     400     reserved            pad to 512 bytes
```

### SMI Entry (64 bytes, fixed)

```
Offset  Size    Field               Description
------  ----    -----               -----------
0       8       file_id             u64
8       4       segment_count       u32
12      4       reserved            u32, must be 0
16      8       extent_offset       u64, offset into extent pool
24      8       extent_count        u64
32      32      reserved            Must be 0
```

### Extent (32 bytes, fixed)

```
Offset  Size    Field               Description
------  ----    -----               -----------
0       8       start_lba           u64
8       8       length_blocks       u64
16      4       seg_start_index     u32
20      4       seg_count           u32
24      8       reserved            Must be 0
```

### SMI On-Disk Layout

```
[SMI HEADER 512B]
[ENTRY TABLE: entry_count × 64B]
[EXTENT POOL: flat array of 32B extents]
[zeroed padding to end of IR region]
```

### IR Copy Selection (on mount)

```
1. Read BH at LBA 0 — verify BLAKE3
2. Use BH to locate IR1, IR2, IR3
3. Load SMI header from each IR — verify BLAKE3
4. Discard invalid copies
5. Among valid copies, choose highest generation number
6. Load full SMI + DHT from winning IR — verify full BLAKE3
7. Synchronize remaining IR copies to match winner (lazy, background)
8. Mount
```

### SMI Rebuild (all IR copies invalid)

```
1. Full segment scan of Data Region
2. Find all segments by magic "ResFSSEG"
3. Verify BLAKE3 of each candidate
4. Filter: IS_COMMITTED set, IS_DELETED not set
5. Group by file_id
6. Sort each group by seg_index
7. Build extents from contiguous runs
8. max_file_id = max(file_id) across all segments
9. file_counter = max_file_id + 1
10. generation = 1 (or old_max + 1 if any valid partial IR found)
11. Build used_blocks bitmap from occupied LBAs
12. Write new SMI + DHT into all three IR regions
13. Mount
```

---

## DHT — Directory Hash Table

The DHT is the namespace acceleration layer: `(parent_dir_id, name) → file_id`.
It is a cache. Destroying it loses nothing — rebuilding from directory
segments is always possible.

### DHT Header (512 bytes, fixed)

```
Offset  Size    Field               Description
------  ----    -----               -----------
0       8       magic               "DHT0\0\0\0\0"
8       4       version             u32
12      4       reserved            u32, must be 0
16      8       generation          u64, same as SMI generation in same IR
24      8       entry_count         u64
32      8       bucket_count        u64
40      8       bucket_table_offset u64, relative to DHT start
48      8       overflow_offset     u64, relative to DHT start
56      32      blake3_root         BLAKE3 of entire DHT body
88      424     reserved            pad to 512 bytes
```

### DHT Entry (64 bytes, fixed)

```
Offset  Size    Field               Description
------  ----    -----               -----------
0       8       hash                u64 = BLAKE3(parent_dir_id || "/" || name)[0..7]
8       8       parent_dir_id       u64
16      8       file_id             u64
24      4       name_len            u32
28      4       flags               u32
32      32      name_blake3         BLAKE3 of name (collision detection)
```

### DHT Hash Function

```c
// hash = first 8 bytes of BLAKE3(parent_dir_id || "/" || name)
uint8_t input[8 + 1 + name_len];
memcpy(input, &parent_dir_id, 8);
input[8] = '/';
memcpy(input + 9, name, name_len);

uint8_t digest[32];
blake3(input, sizeof(input), digest);
uint64_t hash;
memcpy(&hash, digest, 8);
```

### DHT Bucket Addressing

```
bucket_index = hash % bucket_count
collision    → overflow chain
```

### DHT Rebuild

```
1. Scan all IS_DIRECTORY segments (IS_COMMITTED set, IS_DELETED not set)
2. For each directory segment, read all entries
3. Skip ENTRY_DELETED entries
4. For each live entry: insert (parent_dir_id, name) → file_id into DHT
5. Write new DHT into all three IR copies
```

### DHT On-Disk Layout

```
[DHT HEADER 512B]
[BUCKET TABLE: bucket_count × 8B (offsets into overflow area)]
[OVERFLOW CHAIN: DHT entries, 64B each]
[zeroed padding to end of IR region]
```

---

## Index Region Layout

Each Index Region contains exactly one SMI and one DHT.
SMI and DHT within the same IR share the same generation number.
Both are validated together on mount.

All three IRs are identical in size (`ir_size` from BH).
The size is fixed at mkfs and never changes.

```
[Index Region N]  (ir_size blocks total)
    [SMI Header 512B]
    [SMI Entry Table: entry_count × 64B]
    [SMI Extent Pool: flat array of 32B extents]
    [DHT Header 512B]
    [DHT Bucket Table]
    [DHT Overflow Chain]
    [zeroed padding]
```

IR size recommendation at mkfs: `max(MIN_IR_BLOCKS, total_blocks / 1000)`.
Minimum `MIN_IR_BLOCKS = 256` (1MB). This accommodates growth without
requiring a filesystem resize operation.

---

## Write Path (normative)

ResFS is a pure CoW filesystem. No journal. No replay.
Power loss at any step leaves the filesystem in a consistent state.

### CREATE

```
1. counter = atomic_increment(smi.file_counter)
   new file_id = counter
2. Write segment(s) to free blocks in Data Region
   IS_COMMITTED not set
3. fsync
4. Set IS_COMMITTED on new segment(s)        ← atomic commit point
5. Update SMI: add file_id → extents
6. Write new directory segment (CoW: new SEG for parent dir)
   Set IS_COMMITTED on new dir segment
   Clear IS_COMMITTED on old dir segment
7. Update DHT: insert (parent_dir_id, name) → file_id

Power loss before step 4:
  → segments not committed → invisible → clean state

Power loss after step 4:
  → file exists and is recoverable
  → SMI/DHT stale → rebuilt from scan on next mount
```

### WRITE / APPEND

```
1. Write new segment(s) to free blocks (IS_COMMITTED not set)
2. fsync
3. Set IS_COMMITTED on new segment(s)        ← atomic commit point
4. Clear IS_COMMITTED on old segment(s)
   (or set IS_SNAPSHOT_SEG if under active snapshot)
5. Update SMI extents

Power loss before step 3:
  → new segments invisible → old version survives intact

Power loss after step 3:
  → new version live
  → old segments cleaned by GC on next mount
```

### DELETE

```
1. Set IS_DELETED on SEG 0 of file           ← atomic commit point
2. Remove DHT entry
3. SMI cleanup deferred to resfs-gc

Power loss after step 1:
  → file is deleted (IS_DELETED on SEG 0 is authoritative)
  → DHT/SMI stale → rebuilt correctly on next mount
```

### RENAME

```
1. Write new directory segment (CoW): old entry tombstoned, new entry added
2. Set IS_COMMITTED on new dir segment       ← atomic commit point
3. Clear IS_COMMITTED on old dir segment
4. Update DHT: remove old entry, insert new entry

Power loss before step 2:
  → new dir segment not committed → old name survives

Power loss after step 2:
  → new name committed
  → DHT stale → rebuilt from directory segments
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

Allocator strategy: implementation-defined. Recommendation: prefer
contiguous allocation for large files; pack small files together.

---

## GC — Garbage Collection

GC frees blocks occupied by dead segments (no IS_COMMITTED, no IS_SNAPSHOT_SEG).
GC is lazy — it never blocks mount or normal operation.

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
```

### GC Algorithm (resfs-gc)

```
1. Scan all blocks in Data Region
2. For each block containing "ResFSSEG" magic:
   a. IS_COMMITTED not set AND IS_SNAPSHOT_SEG not set → dead, free block
   b. IS_DELETED on SEG 0 → scan all segments of this file_id
      for each: IS_SNAPSHOT_SEG not set → dead, free block
3. Update in-memory free bitmap
4. Update used_blocks in SMI header
5. Write updated SMI to all three IR copies
```

### Snapshot GC

```
1. Mark:  set is_deleted = 1 in Snapshot Region entry
          snapshot invisible immediately, no segments touched
2. Sweep: resfs-gc finds all IS_SNAPSHOT_SEG with this snapshot_id
          verifies snapshot is_deleted = 1
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
2. Locate IR1, IR2, IR3 from BH
3. Load SMI header from each IR — verify BLAKE3
4. Select IR with highest valid generation
5. Load full SMI + DHT from winning IR — verify full BLAKE3
6. Build free space bitmap from SMI extents
7. Mount
```

### BH Lost or Corrupted

```
1. Check EOP at last_lba:
   if EOP valid → use IR hints from EOP to locate IRs
   if EOP invalid → brute-force scan for "SMI0" magic
2. Load SMI headers, verify BLAKE3
3. Select highest valid generation
4. Proceed as normal mount from step 5
5. Optionally reconstruct and rewrite BH at LBA 0
```

### Two-Layer Recovery

```
Layer 1 — valid IR exists:
  Load SMI+DHT from best IR
  Mount immediately
  → seconds

Layer 2 — all IRs destroyed:
  Full O(n) scan of Data Region
  Find all segments by "ResFSSEG" magic
  Group by file_id, sort by seg_index
  Reconstruct files, rebuild SMI+DHT
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
10. Reconstruct SMI and DHT
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

| Tool              | Function                                             |
|-------------------|------------------------------------------------------|
| `mkfs.resfs`      | Format partition: write BH, IR1/2/3, EOP            |
| `resfs-mount`     | Mount via FUSE (Linux) or macFUSE (macOS)            |
| `resfs-recover`   | Full disk scan → reconstruct all files               |
| `resfs-verify`    | Verify BLAKE3 integrity of all segments              |
| `resfs-visualize` | ASCII visualization of segment and free space layout |
| `resfs-snap`      | Create, list, restore, delete snapshots              |
| `resfs-gc`        | Garbage collect dead segments and snapshot remnants  |
| `resfs-export`    | Extract raw file or recovery container from ResFS    |
| `resfs-import`    | Import from ext4/NTFS/exFAT/APFS to ResFS           |
| `resfs-label`     | Get or set filesystem label (rewrites BH)            |

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

| Platform            | Suitable   | Notes                               |
|---------------------|------------|-------------------------------------|
| Embedded / IoT      | ✅ Primary | Small disks, power loss tolerance   |
| Video recorders     | ✅         | Partial file recovery on power loss |
| Routers / cameras   | ✅         | Configs, logs, small files          |
| Desktop             | ✅         | CoW makes it fully viable           |
| Server              | ✅         | CoW + snapshots                     |
| Linux               | ✅         | Via FUSE mount                      |
| macOS Intel         | ✅         | Via macFUSE                         |
| macOS Apple Silicon | ⚠️        | Requires disabling Secure Boot      |
| Windows             | ✅         | Via WinFSP                          |
| Custom kernel       | ✅ Target  | libresfs portable, no OS deps       |

---

## Implementation Roadmap

### Phase 1 — libresfs core

**Criterion: create 10k files, kill -9, recover everything via full scan**

- [ ] `disk.img` creation: BH + IR1/2/3 (fixed size) + EOP + Data Region
- [ ] Bootstrap Header: read / write / verify BLAKE3
- [ ] EOP: write / verify / use in recovery
- [ ] Segment: read / write / verify BLAKE3 (header + footer)
- [ ] CoW write path: IS_COMMITTED as atomic gate
- [ ] Root directory: file_id=1, RESFS_ROOT signature
- [ ] SMI: on-disk format, read, write, rebuild from segments
- [ ] DHT: on-disk format, read, write, rebuild from directory segments
- [ ] IR copy selection: highest valid generation wins
- [ ] Free space bitmap: build from SMI extents on mount
- [ ] GC: lazy dead segment collection
- [ ] Recovery: full disk scan → rebuild SMI+DHT → mount
- [ ] `resfs-recover` proof of concept on disk.img
- [ ] Corruption test suite: random corruption, kill -9, partial writes

### Phase 2 — POSIX-ish API + Mount Simulator

**Criterion: full disk scan → rebuild FS → mount → read all files**

- [ ] `open()`, `read()`, `write()`, `readdir()` over libresfs
- [ ] CLI mount simulator (no kernel required)
- [ ] `resfs-visualize` ASCII segment map
- [ ] Benchmark vs ext4

### Phase 3 — Advanced Features

- [ ] xattr full implementation
- [ ] ACL full implementation
- [ ] Snapshot management + GC
- [ ] Sparse file full implementation

### Phase 4 — FUSE Bridge

- [ ] `resfs-mount` via libfuse / macFUSE
- [ ] Bridge between POSIX API and kernel VFS

### Phase 5 — Kernel FS

- [ ] Segment reader/writer in kernel space
- [ ] SMI rebuild logic in kernel
- [ ] Directory resolution in kernel VFS
- [ ] Power-loss atomicity testing
- [ ] Bare metal port (no OS dependency in libresfs)

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
Each IR = 1 SMI + 1 DHT. BH carries fs_uuid, fs_label, IR locations.
SMI header carries file_counter, used_blocks. CoW write path formalized.

### v1.2
- file_id: counter+CSPRNG (128-bit) → plain u64 monotonic counter
  Saves 8 bytes per segment header and footer (~4GB per 1TB disk)
- seg_total field removed — computable from file_size, was redundant
- file_size = 0 defined for IS_DIRECTORY (size is meaningless)
- Root directory: file_id=1, parent_dir_id=0, "RESFS_ROOT\0" signature
- EOP (End Of Partition) added: physical boundary marker at last_lba
  Contains "ResFsEOP" magic, partition bounds, IR hints, BLAKE3
  EOP_TAIL: human-readable "END OF RESFS PARTITION" at block end
  EOP is optional, advisory only, never outranks segments or BH
- IR regions are fixed-size (allocated at mkfs), never grow
  ir_size field added to BH; unused IR space zeroed
- SMI entry: 128B → 64B (file_id shrunk, reserved fields removed)
- DHT entry: 128B → 64B (file_id/parent_dir_id shrunk to u64)
- Extent: 64B → 32B (file_id removed, reserved trimmed)
- Segment header: 80B → 64B, footer: 40B → 24B (file_id shrunk)
- Data region per segment: 3976B → 4008B (overhead 2.93% → 2.15%)
- GC section formalized: lazy, background, never blocks mount
- Free space model: in-memory bitmap only, not stored on disk
- SMI entry flags → reserved (undefined, will be specified later)
- ACL subject_id: 16B → 8B (aligned with u64 file_id)
- Snapshot region: fixed location after IR1, snap_size in BH

---

*ResFS Specification v1.2*

*Author: Andrei Kovalenko*

*License: PGSL-NC*
