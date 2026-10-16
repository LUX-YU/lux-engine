# N0 independent verification

Input I: `77d007f63fbb8df6754683d51a0297ad54199af7`.
Independent clone: `E:/RenderV2Rebuild/V12/n0-verification`, created with `git clone --no-hardlinks --no-checkout`
and checked out detached at exact I. No source repairs were made there.

`cmake -DLUX_SOURCE_DIR=<clone> -P <clone>/cmake/ValidateTrackedSnapshot.cmake` passed.
The new source-header probe rejected `lux/engine/render/AbsentProbe.hpp` with a nonzero result.
The implementation's existing Error.hpp probe succeeded. N0 has no production C++ change.
719 frozen blobs, all three protected user-file hashes and the two clean dependency SHAs are recorded
in snapshot.json. Source algorithm and current consumer inventories are indexed in N0-evidence-index.json.

The same b427 binaries ran four times per scenario: Native, fixed-work Alias, complete Command.
All 12 runs succeeded; the Alias runs each verified 16 uploads, 256 MiB, 256 chunks and 4096 draws.
All raw CSVs and stdout remain outside the source tree, identified by hashes in the evidence index.
This baseline has observable run-to-run tail variation and does not resolve historical performance PARTIAL.

N0 baseline/protection/planning gate PASS. Production stages N1–N7 have no new qualification yet.
