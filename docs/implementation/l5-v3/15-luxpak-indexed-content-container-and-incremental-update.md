# LuxPak Indexed Content Container、VFS Path Index 与 Incremental Update

Status: **Normative Product Storage / Pak Design — 2026-09-03**

Parents:

- `03-asset-vfs-filewatch.md`
- `07-implementation-roadmap-and-gates.md`
- `08-normative-execution-contract.md`
- `09-product-runtime-vfs-and-async-script.md`

Related:

- `14-plugin-package-and-extension-composition.md`

> 本文件冻结 Lux shipping cooked-content container 的逻辑模型、VFS/Asset lookup index、immutable patch/update 语义与 PAK-0 benchmark gate。它不批准 encryption/DRM，也不把 LuxPak 变成新的 AssetManager、VFS 或网络下载 runtime。

---

## 1. Core definition

LuxPak is **not** a generic filesystem archive.

LuxPak is:

> **an immutable, cook-time indexed, chunked content container backing an ordinary VFS/Asset provider.**

The ownership relation is:

```text
AssetVfs / AssetVfsView
        ↓
PakProvider / other IAssetProvider
        ↓
LuxPak TOC + payload
```

`AssetVfs` remains the authoritative mounted namespace/control plane. `LuxPak` is only one storage representation used by a provider.

MUST NOT:

```text
LuxPak become AssetVfs
LuxPak become AssetManager
Asset loading know package-file internals
Script know LuxPak/Chunk/TOC types
one private VFS per pak/plugin
```

---

## 2. Logical content model

The frozen logical relation is:

```text
AssetId -------------------┐
                           ↓
Canonical VFS Path ---> ContentEntry
                           ↓
                        Segment[]
                           ↓
                         Chunk[]
                           ↓
                     physical bytes
```

The roles are distinct:

```text
AssetId
    stable semantic asset identity

Canonical VFS Path
    filesystem-style virtual address used by VFS resolve/enumerate/path operations

ContentEntry
    packaged logical object; may be a typed Asset or generic/raw VFS blob

Segment
    logical subrange/subresource mapping from one ContentEntry into storage chunks

Chunk
    physical read/compression/integrity/incremental-distribution unit
```

An Asset MUST NOT be assumed to be exactly one physical blob forever.

Examples that the model must naturally permit:

```text
Texture
    metadata
    coarse/fine mip groups

Mesh
    metadata
    LOD0 / LOD1 / LOD2

World
    cell/partition segments

Audio
    header + stream blocks
```

The exact segmentation policy belongs to the Cooker/toolchain, not the runtime Pak format.

---

## 3. Physical high-level layout

The v1 logical layout is MCAP-inspired in one narrow sense: payload may be written first, followed by a read-ready index/summary and a fixed footer that locates it.

Conceptually:

```text
+----------------------------------+
| LuxPak Header                    |
| magic / format version / flags   |
| package identity/build metadata  |
+----------------------------------+
| Payload Chunk 0                  |
+----------------------------------+
| Payload Chunk 1                  |
+----------------------------------+
| ...                              |
+----------------------------------+
| Pak TOC                          |
|   AssetId persisted hash index   |
|   VFS persisted prefix index     |
|   ContentEntry[]                 |
|   SegmentEntry[]                 |
|   ChunkEntry[]                   |
|   tombstones                     |
|   optional cold/debug sections   |
+----------------------------------+
| Fixed Footer                     |
|   TOC offset / size              |
|   TOC integrity                  |
|   format revision                |
+----------------------------------+
```

Runtime mount MUST NOT scan payload to discover entries.

Logical `PakToc + PakData` separation is frozen. Whether v1 physically uses:

```text
one file with embedded TOC/footer
or
small sidecar TOC + large data file
```

is a PAK-0 benchmark decision. Both must use the same logical TOC schema rather than become two semantic formats.

---

## 4. TOC is a read-ready index image

The TOC is produced at cook time and consumed directly at runtime.

MUST:

```text
fixed/explicit wire widths and endianness
explicit alignment
contiguous index/table sections
bounds-checked offsets/counts
integrity validation before publication
mmap/read-friendly representation
no dependence on C++ STL object memory layout
```

MUST NOT:

```text
dump std::unordered_map memory
serialize implementation-specific pointers
mount then allocate one C++ object per asset merely to rebuild the TOC
mount then rebuild an unordered_map/radix tree from all paths
scan every payload record to reconstruct the directory
```

Cook time may spend extra work to optimize index layout because runtime is immutable/read-mostly.

---

## 5. AssetId exact lookup — persisted flat hash

The primary typed-asset hot path is:

```text
AssetId
   ↓
persisted flat hash table
   ↓
ContentEntry index
```

v1 target architecture is a cook-time generated flat open-addressing table or an equivalent explicit persisted flat-hash layout.

Conceptual representation:

```text
AssetHashHeader
    algorithm/version
    seed
    capacity
    count
    max-probe metadata as required

AssetHashSlot[]
    compact fingerprint / occupancy metadata
    ContentEntryIndex
```

The full canonical `AssetId` remains available in the authoritative entry and MUST be verified when required for collision safety.

Cooker MAY choose seed/capacity repeatedly until approved load-factor/probe constraints are satisfied.

Runtime MUST NOT rebuild `std::unordered_map<AssetId,...>` at mount.

Expected lookup complexity:

```text
O(1) average
```

with a small number of contiguous/cache-friendly probes under the qualified table policy.

Exact slot representation, probing strategy, hash algorithm, load factor and fingerprint width are PAK-0 benchmark/freeze parameters.

---

## 6. VFS path lookup — persisted prefix-search index, not full-path hash

VFS path semantics are different from AssetId semantics.

VFS needs:

```text
exact resolve/open by canonical virtual path
prefix/directory enumeration
folder/tree traversal
patch shadow/tombstone behavior
pathOf / diagnostics where required
```

Therefore the canonical v1 direction is:

> **a cook-time generated persisted compact prefix-search index**

rather than a second full-path hash table.

Candidate physical layouts for PAK-0 are:

```text
compact radix tree / Patricia-style compressed trie
or
qualified double-array trie / equivalent compact static trie
```

The runtime MUST consume the finalized serialized index directly. It MUST NOT read all paths and construct a heap/pointer radix tree after mount.

A compact representation may use arrays such as:

```text
Node[]
Edge[]
LabelBytePool[]
Value[]
```

or an equivalent static trie layout.

No pointer-owned node graph is part of the wire format.

---

## 7. Canonical virtual path bytes are a prerequisite

A persisted VFS prefix index is only correct if cooker and runtime agree on exactly the same virtual-path byte representation.

Before the final Pak wire is frozen, the VFS contract MUST freeze canonicalization for at least:

```text
root syntax
separator syntax
`.` / `..` policy
trailing separator policy
UTF-8 validity/normalization policy
case-sensitivity/case-folding policy
```

The cooked index MUST use canonical VFS bytes, not host-OS filesystem spelling.

Windows/macOS/Linux host filesystem behavior MUST NOT silently change shipping virtual-path identity.

This document does not invent a new path-normalization policy; it requires the existing VFS owner to freeze one shared by cooker and runtime before PAK-1 shipping closure.

---

## 8. VFS prefix index complexity

Let:

```text
L = canonical path byte length
P = prefix byte length
K = number of returned descendants/entries
```

A qualified persisted radix/trie index targets:

```text
exact path lookup       O(L)
prefix location         O(P)
enumeration             O(P + K)
```

A full-path hash also has to read/hash O(L) input bytes and does not naturally support prefix enumeration without a second directory structure.

For Lux, VFS semantic fit and one-index resolve/enumerate behavior are more important than forcing exact-path lookup into the same hash structure as AssetId.

Normal typed Asset hot paths SHOULD resolve/use `AssetId` and therefore bypass path traversal entirely.

Repeated path users SHOULD resolve once to an approved stable/resolved VFS capability where the owner contract permits, rather than repeatedly traversing the same path every frame.

---

## 9. Reverse path relation / `pathOf`

Because `AssetVfsView` has path-oriented semantics in both directions, the Pak representation MUST preserve enough information to implement approved `pathOf(AssetId)` behavior without a runtime-built reverse string map.

A qualified design may use, for example:

```text
Asset/Content entry -> canonical path terminal/node reference
```

and reconstruct canonical path bytes through persisted parent/label information, or an equivalent explicit reverse reference.

If aliases/multiple paths for one AssetId are supported by the VFS contract, the canonical/primary path rule MUST be explicit. PAK-0 MUST NOT silently choose one based on hash/table insertion order.

---

## 10. ContentEntry

A conceptual entry contains stable storage facts rather than runtime object ownership:

```text
ContentEntry
    canonical AssetId when typed/addressable as Asset
    AssetTypeId or equivalent type identity when applicable
    ContentHash
    first SegmentEntry index
    segment count
    flags
    optional canonical-path terminal/reference
```

The exact struct is not frozen by this document.

MUST NOT store:

```text
runtime Asset pointer
shared_ptr
provider pointer
C++ vtable/type_info address
editor object pointer
```

AssetId and canonical VFS path may both resolve to the same ContentEntry.

A path rename with unchanged AssetId/content therefore need not retransmit payload content.

---

## 11. SegmentEntry

A Segment maps a logical ContentEntry subresource/range into a storage Chunk.

Conceptually:

```text
SegmentEntry
    ChunkEntry index
    offset within uncompressed logical chunk content
    logical size
    segment flags / semantic subresource metadata only if later approved
```

A small ContentEntry may occupy one segment inside a shared chunk.
A large/streaming ContentEntry may span many segments/chunks.

Segment metadata MUST remain storage-oriented. Do not turn it into a universal Asset semantic/property system.

---

## 12. ChunkEntry

Chunk is the primary physical storage unit.

Conceptually:

```text
ChunkEntry
    ChunkContentHash
    data offset / physical locator
    stored byte size
    uncompressed byte size
    compression codec
    alignment
    storage flags
    StorageChecksum / stored-byte integrity metadata
```

Chunk size is not globally hard-coded by the logical format.

Cook policy may choose different qualified chunk sizes for random-access, normal, and sequential/streaming groups.

PAK-0 MUST benchmark at least representative sizes such as:

```text
64 KiB
128 KiB
256 KiB
512 KiB
1 MiB
```

and record read amplification, compression ratio, I/O request count and sequential throughput.

---

## 13. Compression policy

Chunk is the independently readable compression unit.

v1 format MUST be able to represent at least:

```text
NONE
one fast random-read codec
one higher-ratio general codec
```

Candidate policy may use LZ4 and/or Zstd, but exact default/thresholds are a benchmark decision rather than a normative guess.

MUST NOT make the entire Pak one monolithic compressed stream whose random access requires decoding unrelated earlier content.

Compression choice is storage representation and MUST NOT change AssetId or logical ContentHash semantics.

---

## 14. ContentHash vs StorageChecksum

Two identities/checks MUST remain semantically separate.

### ContentHash

Represents canonical uncompressed cooked content identity.

Used for:

```text
incremental cooking/update diff
dedup/cache identity
release/update comparison
future content-addressed distribution
```

Changing compression/storage representation alone SHOULD NOT change ContentHash.

### StorageChecksum / stored-byte integrity

Represents/validates bytes physically stored/transferred.

Used for:

```text
corruption detection
download/storage verification
```

Do not use CRC32 alone as the sole content identity for incremental/dedup decisions.

Exact strong ContentHash algorithm/width is a PAK-0 freeze parameter; BLAKE3-class strong content hashing is an example candidate, not yet a normative algorithm choice.

---

## 15. Immutable Base and Patch Paks

Shipping containers are immutable after installation/publication.

MUST NOT rewrite the installed Base Pak in place for normal content updates.

Update model:

```text
Base*.luxpak       lower mount priority
Patch101.luxpak    higher priority
Patch102.luxpak    higher again
```

A Patch Pak contains only the content/index facts needed by the new release.

Patch semantics MUST support:

```text
ADD
REPLACE/OVERRIDE
TOMBSTONE/REMOVE
```

A tombstone is terminal for the relevant lookup in that higher-priority provider layer and MUST prevent an older Base/Patch entry from becoming visible again by fallback.

Exact VFS mount-priority/recency semantics remain owned by AssetVfs.

---

## 16. Path rename/update semantics

`AssetId` and canonical VFS path are different identities.

If an asset is renamed/moved while its semantic AssetId and content remain unchanged:

```text
old path -> tombstone/remove
new path -> same AssetId/ContentEntry semantic content
payload chunks unchanged
```

The updater SHOULD NOT retransmit unchanged payload merely because a virtual path changed.

This is one reason path identity MUST NOT replace AssetId identity.

---

## 17. Release Manifest is separate from runtime Pak TOC

LuxPak solves runtime installed storage.

Release/update distribution uses a separate Release Manifest owned by Product/update tooling.

Conceptually:

```text
ReleaseManifest
    BuildId / release identity
    compatibility/base information
    package list + package integrity
    required ContentHash/ChunkContentHash set or equivalent update inventory
    distribution metadata
```

The exact manifest syntax is NOT frozen here and must integrate with Product Track P rather than become a parallel generic project/plugin manifest.

Do not put CDN URLs/network policy into AssetVfs or Asset loading semantics.

---

## 18. Incremental update model

Update diff is content-aware rather than whole-file binary-diff dependent.

Conceptually:

```text
previous cooked/release content inventory
              +
current cooked content/chunk inventory
              ↓
compare strong content hashes
              ↓
new/changed chunks + new TOC facts + tombstones
              ↓
Patch LuxPak / release payload
```

A platform/store binary delta may be used as an additional distribution optimization, but LuxPak correctness and update efficiency MUST NOT depend on it.

A small logical content change SHOULD NOT require downloading an unrelated full multi-gigabyte Base Pak merely because physical offsets changed.

---

## 19. Update maturity levels

### PAK-1 shipping closure

The minimum approved shipping update model is:

```text
immutable Base Pak(s)
immutable Patch Pak(s)
ADD / REPLACE / TOMBSTONE
content-hash release diff
atomic patch install/publication
bounded patch overlay chain
```

The client may download a complete Patch Pak. Because the Patch Pak contains only changed/new content, this already avoids full-game replacement.

### Later chunk-aware distribution

When real product/CDN data proves it useful, update distribution MAY address/download individual missing chunks by strong content identity.

Preferred relation:

```text
Updater/Launcher
    downloads/verifies missing chunks
    materializes an immutable self-contained Patch Pak
        ↓
Runtime
    mounts ordinary PakProvider
```

Normal runtime `loadAsset()` MUST NOT become an implicit HTTP/CDN request merely because chunk-aware delivery exists.

On-demand install/cloud streaming is a separate future provider/product requirement.

---

## 20. Bounded patch chain / compaction

Patch layers MUST NOT grow without bound.

Product/update policy MUST periodically produce a new compacted/rebased Base content set when thresholds are reached.

Thresholds may depend on measured:

```text
number of mounted patch generations
total patch bytes
lookup-layer cost
installation/storage policy
```

Exact maximum layer count is a benchmark/product policy decision, not a magic value in the Pak wire format.

A small bounded number of immutable providers is expected to keep overlay lookup cost low.

---

## 21. Multiple physical containers are allowed

A shipping product is not required to place all game content into one enormous file.

Product/cook policy may produce multiple containers for reasons such as:

```text
startup/core content
maps/regions
language packs
optional/DLC content
server/runtime product closure
platform-specific content groups
```

The runtime still observes one mounted VFS namespace.

Physical package grouping MUST NOT become the minimum logical update unit; changed content is identified through cooked content/chunk identity.

---

## 22. Locality belongs to Cooker policy

Pak format supplies Segment/Chunk mechanisms.

Cooker decides physical layout/locality.

Assets commonly loaded together SHOULD be packable into nearby/contiguous chunks so the I/O layer may coalesce/prefetch reads.

Do not encode one universal scene/load-set policy into the Pak wire format.

The format must permit Toolchain evolution of:

```text
load groups
streaming groups
platform layout policies
prefetch/coalescing strategies
```

without changing VFS/Asset semantic identities.

---

## 23. Development vs Shipping metadata

The logical runtime identity/index model stays the same, but cold/debug metadata may differ by product profile.

Development may retain more:

```text
human-readable diagnostics
source/cook provenance
full type/debug names
extra integrity/debug metadata
```

Shipping may strip data not required for runtime correctness.

The persisted path prefix index itself necessarily contains enough canonical path label bytes to resolve/enumerate the paths it serves; do not claim that all path bytes can be stripped while retaining runtime path lookup.

Avoid a duplicate full-path StringTable unless a real diagnostic/tooling consumer needs it.

---

## 24. Encryption / signing status

Encryption/DRM/key management is explicitly **NOT DONE** in LuxPak v1 architecture work.

Do not implement now:

```text
CryptoManager
PakSecurityService
KeyRegistry
AES/key-distribution framework
DRM policy
```

The wire format SHOULD reserve storage/security flags with only the approved initial mode:

```text
SecurityMode::NONE
```

A v1 reader encountering an unsupported non-NONE mode MUST fail closed.

Hashing/integrity semantics are designed so a future security wave may insert:

```text
canonical uncompressed chunk
    ↓ ContentHash
compress
    ↓
encrypt/authenticate
    ↓ stored bytes / authentication metadata
```

without changing AssetId/VFS/update identity.

This reservation is not permission to implement encryption in PAK-0/PAK-1.

---

## 25. Runtime mount/read complexity contract

### Mount

For payload size `D` and TOC size `T`:

```text
payload scan cost: O(1) / none
TOC validation/publication: O(T) worst-case if fully validated/read
```

A mmap/lazy-paged implementation may make initial physical I/O closer to footer + required TOC pages.

Mount MUST NOT be O(D).

Mount MUST NOT allocate/rebuild one index object per asset/path merely to become usable.

### AssetId exact lookup

Target:

```text
O(1) expected persisted-flat-hash lookup
```

plus table/entry memory accesses.

### VFS exact path

Target:

```text
O(L)
```

for canonical path length `L` using the persisted radix/trie structure.

### VFS prefix enumeration

Target:

```text
O(P + K)
```

for prefix length `P` and returned entry count `K`, excluding higher-level overlay merge/shadow work proportional to the mounted providers/results actually involved.

These are complexity contracts, not machine-specific nanosecond SLAs.

---

## 26. Read performance model

After lookup, the intended typed Asset path is short:

```text
AssetId
    ↓ persisted hash
ContentEntry
    ↓ Segment range
SegmentEntry
    ↓ ChunkEntry
async storage read
    ↓
chunk decompress
    ↓
logical segment bytes
    ↓
typed Asset decode / later GPU upload as required
```

In realistic loads, storage latency, decompression, decode and GPU upload are expected to dominate the few TOC index accesses.

PAK-0 optimization effort SHOULD therefore prioritize:

```text
read amplification
chunk size/policy
locality/layout
I/O coalescing
compression throughput/ratio
TOC memory/cache behavior
```

rather than optimizing a path lookup that normal AssetId hot paths do not use.

---

## 27. PAK-0 format/benchmark gate

Before freezing exact v1 wire structs, implement a minimal cooker/reader prototype and benchmark representative synthetic + real cooked data.

PAK-0 MUST compare/freeze at least:

```text
AssetId persisted hash slot/probe layout
AssetId hash algorithm/seed/load factor
strong ContentHash algorithm/width

VFS compact radix vs double-array/static-trie layout
exact-hit / exact-miss / prefix-enumerate workloads
path-index memory footprint

embedded TOC vs sidecar TOC
TOC mmap/read/validation behavior

64/128/256/512 KiB / 1 MiB representative chunk policies
NONE / fast codec / higher-ratio codec policy
random small-read amplification
large sequential/load-group throughput

Base + small Patch overlay lookup
ADD / REPLACE / TOMBSTONE correctness
content-hash diff/update size
path rename without payload retransmission
```

Benchmark with large entry counts (hundreds of thousands to approximately one million where practical) and representative real cooked assets.

Do not turn one workstation timing into a normative global SLA. Freeze the physical layout based on complexity, cross-platform reproducibility and measured product workloads.

---

## 28. PAK-1 shipping gate

PAK-1 may be called shipping-ready only when all of the following are proven:

```text
canonical VFS path representation frozen
cross-platform deterministic cook output or explicitly explained deterministic sections
TOC corruption/bounds validation fail closed
persisted AssetId index qualified
persisted VFS prefix index qualified
async PakProvider/AssetRead integration qualified
chunk compression/read qualified
Base/Patch/tombstone overlay qualified
release diff produces incremental patch content
atomic install/publication/rollback policy approved
bounded patch/rebase policy approved
installed product uses no runtime index rebuild
```

Encryption is not a PAK-1 gate unless a real product security requirement separately approves it.

---

## 29. Product/Plugin relation

Source Plugin Package runtime assets use the same product-wide LuxPak/VFS model as project/engine assets.

A Plugin Package MUST NOT create:

```text
PluginPak runtime manager
PluginVfs
private Plugin asset database
parallel update protocol
```

Product Track P eventually decides which project/plugin cooked content contributes to which physical container(s).

Exact Plugin/project manifest syntax remains outside this document.

---

## 30. Global STOP conditions

STOP for architecture review if implementation appears to require:

```text
runtime scan of Pak payload to build directory/index
runtime rebuild of unordered_map for every AssetId
runtime heap construction of path radix nodes
full-path hash as the only VFS structure while enumerate/prefix semantics require a second ad-hoc directory SSOT
Asset == exactly one physical blob as a permanent format invariant
one monolithic compressed stream for the whole Pak
in-place mutation of installed Base Pak for normal updates
fallback through a higher-priority tombstone
whole-game replacement for a small logical content change when cooked chunk identity is unchanged
network/CDN requests hidden inside normal loadAsset()
Pak-specific AssetManager/ServiceRegistry
path identity replacing AssetId semantic identity
compression hash conflated with canonical ContentHash
implementation-specific STL memory image in wire format
unbounded patch chain without compaction policy
encryption/DRM framework introduced during PAK-0 without separate approval
```

---

## 31. Frozen principles

> **LuxPak is an immutable indexed content container backing a VFS provider, not a filesystem or Asset manager.**

> **Typed Asset lookup uses a cook-time persisted flat hash on AssetId; VFS path lookup/enumeration uses a cook-time persisted compact prefix index such as radix/static trie. Neither index is rebuilt at runtime.**

> **Content is modeled as ContentEntry → Segment → Chunk so compression, streaming and incremental delivery can evolve without changing Asset identity.**

> **Shipping updates are immutable Patch Paks with ADD/REPLACE/TOMBSTONE semantics and content-hash diffing; Base content is not rewritten in place.**

> **Encryption is intentionally deferred. The wire only reserves a fail-closed future security mode while keeping ContentHash and stored-byte integrity distinct.**
