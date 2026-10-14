# Architecture Convergence R0 implementation

Baseline: `b05904dc71f91be2c89964461430ad924ee97e2a`.
Input: `Render_V2_Architecture_Convergence_R1_2026-10-14.zh-CN.md`, SHA-256
`102721175cd18fde01757b420b03d31bf99c9207b70e2416d315be4e668fd703`.
The complete authorized sequence is R0 through R6. This note qualifies only R0;
Native ledger, completion stream, open Operations, asynchronous compilation and lowering remain pending.

## Authorship

FeatureAssembly now owns numerical contribution intervals established by the actual Builder appends.
Each interval retains the input selection index, first Pass and count, including zero-Pass Features.
Canonical sorting does not alter this index. It retains no callback, configuration, string or Definition borrow.
Runtime keeps the already resolved registration beside each selection and validates that each returned Program
names a Pass in that selection's actual interval. A foreign Program produces kFeatureOwnership; a duplicate
produces kFeatureDuplicate. The Native Compiler reports kFeatureContract and the live Pass index for missing
Programs. The single G4 Analysis remains unchanged; no new Graph or runtime registry is introduced.

## Retirement commit point

Detach, Resize and batch Remove perform capacity admission before cancelling readbacks or changing groups.
The sole Server owner serializes admission and commit: no submit/retire can interleave; queue completion can
only reduce the needed capacity. Every group points to a DeviceSession-owned context. Candidate Resize
preparation does not submit or retire work. Thus release cannot exhaust capacity after this admission.
Readbacks are then cancelled while their context is still alive, followed by retirement and group publication.
The existing Native boundary still validates ownership and returns recoverable errors for external callers.
The configurable retirement capacity is validated once at Server construction and passed to DeviceSession.

## Tests and evidence

Programs.cpp uses deliberately different selection/canonical orders and checks foreign, duplicate and missing
live Programs, followed by successful recovery without changing the prior compiled Effect count.
Shutdown.cpp fills a one-slot retirement queue with a real submitted GPU context. It holds completion
observation through the existing Native fault seam; no new queue or fake submission is used. Detach, Resize,
RemoveView, RemoveScene and RemoveEffect fail without cancelling the outstanding readback. Releasing the
observation gate gives the original 32x32 red pixel result; the next successful resize has epoch 2, proving the
failed resize did not publish an epoch. The original Stop/Requester-close tests remain intact.

Commands use `E:/RenderV2Rebuild/ConvergenceR1/run.ps1` with configure/build/test actions;
build always means `cmake --build <build> --target all -j 4 -- -k 0`, test means serial CTest.
Raw implementation logs live outside source in `E:/RenderV2Rebuild/ConvergenceR1/evidence`.
The separate R0 checker freezes all files outside the explicit R0 list, all 719 Legacy blobs, historical
reports, the entire IPC implementation, and both primary-worktree user modifications.
This is not an independent final R6 acceptance report and does not upgrade any historical qualification.

R0 implementation gate: RelWithDebInfo 46/46; AddressSanitizer 46/46, including real GPU and relocated SDK consumers. Both full second builds report no work. This is implementation-tree evidence; independent batch verification remains required.
