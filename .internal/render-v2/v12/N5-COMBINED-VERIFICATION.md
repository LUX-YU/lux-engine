# N5 combined independent verification

Exact implementation `fae3f9b710842e28d002aff2c91ea4d90a3c573e`, parent `db30b88e064e84975542d72ed4a61197a855c5fe` (Geometry I).
Remote observed `78bcd220815f845466d85d31f994901e4db224ba`; no push. Detached no-hardlinks clean clone `E:\RenderV2Rebuild\V12\n5-combined-verification` has no alternates;
tracked snapshot precedes each fresh configuration. No implementation changes in verification source.

| Configuration | Full tests | Seconds |
|---|---:|---:|
| RelWithDebInfo | 114/114 | 128.54 |
| Debug | 114/114 | 131.22 |
| MSVC ASan | 114/114 | 174.56 |

Every configuration: full all -j4 -- -k0, second build no work, serial CTest -R .; all102 original names
preserved. Public C++20/GR- headers, fresh installed and relocated SDK, pure Client without Vulkan,
pure synchronous dependency configuration, external Buffer and CPU/IPC direct dependencies pass.
ASan /fsanitize=address /Zi, alloc_dealloc_mismatch=1. GPU Validation/SyncValidation enabled; Error0,
ASan Error0. Real EOS duplicate-layer warning retained. RTX4070 Laptop; exact driver/API in raw logs.

N5.1 typed configuration and owner requirements, N5.2 live-only assembly/program ownership and
parameter planning, N5.3 existing lglsl emitter/sole private LayoutPlan and SPIR-V relocation,
N5.4 actual stage/entry/block/member/stride and multi-provider parameters/formats, N5.5 real Vertex,
Index, indexed-indirect, Compute and Depth, N5.6 complete owner-safe-point rebinding and explicit
Readback have passed their production tests in this exact combined source. Earlier checkpoint reports
retain their original scope and conclusions; this report does not rewrite them.

Geometry checks actual Compute-produced Vertex/Index/Indirect data and pixels; cold invalid formats,
ranges, strides and Shader IO reject. Depth covers3color formats x2Depth formats x6scenarios, resized
attachments, all pixels/depth, Vertex-only Depth, explicit transfer clear and cold invalid states.
Original Pipeline/Bind/Scope counts, three Queue topologies, LastUse, partial submit, owner lifetime,
failed request recovery and complete Handler/Storage registration regressions remain passing.
The measured record/execute windows retain first-party new=0; these are not full rendering or latency
performance claims. LocalSizeId and multiDrawIndirect count>1 remain explicitly unsupported; the real
single indirect command permits multiple instances. No full GPU Culling/WSI/HDR qualification.

Exact-I64 public headers synchronized with backups/hashes to three prefixes;13 obsolete headers
recorded. Legacy719, main/Layer user modifications, historical evidence, dependencies and whitelist pass.
Implementation failed attempts retained, including test parameter-field mistake and prior numeric oracle
adjustment as described in their unchanged reports. No failed attempt is counted as qualification.

**N5 structural/function gate PASS in this tested scope. N6/N7 NOT_STARTED.** Historical performance
and IPC PARTIAL remain; equivalent-workload/tail analysis still N7. Root TOOLCHAIN historical BLOCKED;
upper Engine/Editor, Linux/Android, TSan NOT_RUN. Android header sync is not build qualification.
GPU timestamp here NOT_MEASURED. Final N7 must independently repeat the complete final-I matrix.

Raw commands, return codes, logs, hashes, SDK backups and failed implementation attempts:
`E:\RenderV2Rebuild\V12\archive\fae3f9b710842e28d002aff2c91ea4d90a3c573e\n5-combined-evidence.zip`. SHA-256 `93feec8ed0f3fa18caeaf7ad28be529a3888eb953ad3fb75731aeb0a912957e5`. Adjacent JSON indexes every archived file.
