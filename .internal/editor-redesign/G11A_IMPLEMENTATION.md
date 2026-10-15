# G11A implementation gates

Baseline: 9a2154371018b082dc2c0270a2c898559d69e213. This is a construction log, not final I/V qualification.

## G11A-0

Package six SHA256 entries and ZIP CRC checked; isolated worktree; Legacy719 blobs and two user SHA256
values match. Native/Runtime/Graph/Feature/Legacy/resource-color source audit recorded in architecture freeze.
Actual NVIDIA families support alias, same-family separate queues and transfer-family EXCLUSIVE testing.

## G11A-1

Queue-aware G10 reserve captures one explicit Session queue's frontier; commit validates that owner and
strictly newer admitted ticket; poll uses that same owner. The old default graphics overload is preserved.
Session completed-release accepts only one of its actual queue owners; image and buffer permits retain their
generation/retired proof. Aliased roles still share one SubmissionQueue and one RetirementQueue.

Release `all -j 4 -- -k 0` succeeded, second all no-work. First complete 65-test execution preserved old 62
and passed Native transfer; two new Runtime variants failed in the old saturation fixture. The old test
assumed four Graphics Views filled the upload queue. A separate Transfer queue remained free, accepted a
request, and its deliberately held completion could never reach the expected capacity failure. Corrected
fixture fills that explicit physical queue with bounded real submissions, without changing production queue
logic, sleeps, timeout or capacity assertion. Both new variants then pass (22.62 / 28.45 seconds).
Original failure log: evidence/a1-first-ctest.log. Successful targeted log is retained by run.ps1.

Native transfer: alias 0:0→0:0; same-family 0:2→0:0; dedicated 1:0→0:0. Actual Image Copy,
EXCLUSIVE release/acquire, timeline wait and readback verify 1024 bytes per case; validation_errors=0,
warnings=1 (existing overlay warning). Runtime transfer tests preserve two Clients, four Views, original
Owner, failure/stop and 1000 complete GPU-ready operation samples. Dedicated sample: 499 Upload completions
while Main pauses 1.5s; Native buffers 3022 creates/3022 destroys; no GPU validation error.
DeferredCompletion 448→456 bytes (one borrowed queue pointer); Request216/Reservation24/Context40,
Permit16/IpcMessage144/IpcReply296 unchanged. This is not Texture performance qualification.

G11A-2..5 and independent V have not yet run. G11A overall is NOT_COMPLETE; no PASS or push authorized
by these internal results alone. G11B–G13 remain NOT_STARTED.
