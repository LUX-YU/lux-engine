# F5 test impact and qualification boundary

Entry: `719f3d06e077e639901ae598f230653c5312c905`.
All changed file Git objects are captured in the final independent closure report;
`F5_NATIVE_IMPACT.md` records each native consumer and lower-layer dependency reason.

| Changed production inputs | Actual consumers | Required qualification |
|---|---|---|
| Native immutable code / invocation split, Sync cells, imported state | NativeGraph tests, Runtime Backend/Frame, hot native consumer | Existing Native GPU oracles, invocation 2/3 FIF, all alias/import/retirement/draw tests, Full ASan, Native benchmark |
| F3 descriptor physical extent argument | Native recipes and relative Target variant allocation | Shader/layout/descriptor regressions, relative resize oracle, fixed Binding benchmark |
| Device extension negotiation, Sync2 binary submit | Real WSI Runtime target | Foundation/fault/submission regressions, actual two swapchain pixel oracle, present completion/resize/stop, SyncValidation |
| Runtime protocol generator input | Existing luxop generator -> RuntimeMessages.ops.hpp -> Runtime/consumers | Actual job/dependency/produced-header hashes, neutral public headers, typed compile negatives, reply/stop/owner tests |
| Runtime publication, frame, Scene/View/Target owners | One actual backend thread and native invocation pools | Scene sharing proof, two Scenes/shared Buffer, different paths/partitions, private GPU histories, 10Hz vs120Hz, no PROGRAM frames, failure/stop/backpressure, warm allocation census |
| Test-only timestamp wrapper | Profiling executable only | Link-map exclusion from production; fixed 24+240 sample baseline, actual queue timestamps and numeric output |

No test obligation is removed. Final qualification runs the original 156 CTest
names plus all F5 tests in both Normal and Full ASan from the exact I clean clone.
Development only rebuilt affected targets. Old production Core/Transport/Graph,
Shader Generator, Legacy, FINAL/RD2 and history are protected by Git tree/blob identity;
their historical evidence is not rewritten. Their tests are retained in final integration.

`verify_f5.py` checks File API targets, actual compilation headers from Ninja deps,
source/link/codegen inputs and hot link maps. Test selection is not based on filename
matching alone. The same closure is checked in Normal and ASan. The original user
checkout snapshots and seven dirty-file byte hashes are checked independently.

GPU/ASan/Normal are required for this stage. Android/Linux/products, full installed
Feature SDK and F7 DataSet data chain are NOT_RUN/out of scope. Three public-header
prefixes are synchronized with backups and exact source hashes; this is not SDK or
Android qualification. Actual driver Device Lost remains NOT_RUN; deterministic
injection traverses the real Runtime failure path after legal test-only GPU drainage.

F4 cold p95 uncertainty remains PARTIAL under the user's entry decision. The fixed
F5 protocol governs hot regression and new Runtime baseline; no retry-until-green.
