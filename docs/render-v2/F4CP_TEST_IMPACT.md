# RD2-F4-C-P test impact and responsibility

Entry: F4-C verification `6342d56caf826504278399e2430a2a4499d9d3c2`.
This work changes no production C++, Shader, public ABI, resource owner or compiler algorithm.

| Changed input | Actual consumer / effect | Required evidence |
| --- | --- | --- |
| `vulkan/test/NativeGraphDraw.cpp` | Existing `render_vulkan_graph_draw` executable; F4Closeout.cmake compiles this TU and links the existing Shader/Native Graph libraries | All five draw modes, UINT16 unchanged, new UINT32 Normal/Full ASan/GPU/SyncValidation; no new shader or command type |
| `F4Closeout.cmake` | Adds `indexed-u32` argument to the existing executable in CTest | Original 155 names retained, exact new name; second all build has no work |
| `verify_f4.py` | Offline independent qualification only; narrow new entry/scope policy and 156 inventory, existing closure checks retained | Real File API, compiler/Ninja headers, libraries, hot link map, generator inputs and protected objects |
| `measure_f4cp.py` | Offline process runner consuming existing full benchmark executables; not linked or generated into production | Frozen protocol, all 372 commands/results, raw pairs/controls, binary identities, uncertainty gates |
| `F4CP_*.md` | Work contract and receipts | Scope, source provenance and review |

The actual compiler source list and generated inputs are compared with the locked F4-C qualification.
All production Git objects must remain identical. Different clean-clone paths may change binary debug
identity: that does not authorize claiming identical executables. Preserve hashes and compare normalized
compile/link command inputs, rather than inferring independence from file names.

Development changes do not trigger repeated unrelated matrices. The final I gets ONE complete independent
Normal and Full ASan qualification as explicitly required for this test change: all 156 tests, including
the original 155, Foundation fault paths, prior C1/C2/C3, generated Shader contracts, Graph oracles,
GPU validation/readbacks, public-header and dependency negatives. Failed attempts remain recorded; a
production or test correction after I requires a new implementation identity and requalification.

The new positive uses the existing F4CIndexed generated schema and vertex/fragment SPIR-V unchanged,
an actual UINT32 index buffer with byte range [8,20), and indices 1,2,3. The same deliberately unusable
dummy vertex, full-screen triangle pixel oracle, Sync2 trace, two/three frame slots and warmed allocation
counter remain. It does not add a second draw fixture, CPU substitute for the GPU or production seam.

Historical qualification and failed performance samples are immutable. New evidence is stored separately
under `archives/lux-engine/RenderV2/F4-C-P/<I>/`. Original user six edits and V2 Operation.hpp are protected
by exact HEAD/branch/status/diff/untracked and byte-hash comparisons to both current preflight and F4-C.
No public header changed, so installation prefixes require identity verification, not new synchronization.

NOT_RUN: product/SDK, Linux/Android, Runtime/DataStorage/Feature work and advanced rendering. These are
outside this authorization. CPU PMU counters are not claimed; earlier access denial is retained. F4's
previously user-deferred native capabilities remain NOT_IMPLEMENTED at their recorded future stages.

Only independent qualification can close the two cold gates. If the fixed experiment remains uncertain,
report PARTIAL with its full confidence intervals and STOP; do not launch more trials or change production.
