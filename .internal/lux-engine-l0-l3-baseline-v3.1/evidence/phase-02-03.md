# Phase 2–3 Qualification Evidence

## Reviewed commits

- Phase 2 metadata-only World: `0b8a46b3`
- Phase 3 storage/wire: `432cdd38`

## Gates

- RelWithDebInfo full target `all -j 4 -k 0`: passed.
- Immediate second build: `ninja: no work to do`.
- CTest: 97/97 passed.
- Installed consumer `lux_world_consumer`: configured, built and executed successfully.
- Installed consumer `lux_world_storage_consumer`: configured, built and executed successfully.
- Retired source/install header `WorldPartitioner.hpp`: removed; no compatibility alias remains.
- Source scan finds no active `WorldPartitionWorkspace` or `WorldPartitionLayoutBuilder(const WorldDescription&)`.

## Phase 2 benchmark

Candidate based on the Phase 1 parent and the Phase 2 working tree:

| Metric | Count | Time | Retained |
|---|---:|---:|---:|
| World root build | 1,000,000 partitions | 22,600 ns | 381 bytes |
| Page lookup | 1,000,000 lookups | 3,461,800 ns | 381 bytes |
| Exact-cover layout build | 100,000 objects / 100 partitions | 28,317,400 ns | n/a |

The root contains one page descriptor and its retained memory is independent from object count and partition count.

## Wire coverage

- root v2 metadata roundtrip and canonical ordering;
- BundleId/Generation/VolumeOrdinal mismatch rejection;
- fixed 80-byte volume header and fixed 64-byte descriptors;
- 64-bit range validation;
- per-chunk SHA-256 and digest mismatch rejection;
- multi-volume/multi-extent partition table records;
- partition payload schema/version/bounds checks;
- no per-object allocation and no object-level IO API.
