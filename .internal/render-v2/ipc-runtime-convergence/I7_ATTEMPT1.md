# I7 attempt 1 — dependency probe correction

Exact attempted I: ed2f80a1cec1bec14b9437e3505e0b1dc5da6ec9. The independent clone passed tracked snapshot and configured a fresh RelWithDebInfo build. Its stricter neutral Server header target failed because CommandHandler.hpp uses IpcMessage, but the target omitted the existing render_protocol_handlers component that publishes that dependency.

The correction is solely the test target's explicit dependency on render_protocol_handlers. It does not link Native into the neutral probe or add a new target/stub. Production Runtime already consumes that component through render_protocol_async. The verification clone remains unchanged. This correction creates a new implementation commit; independent verification restarts at that exact commit in a new source/build tree. Failed logs are retained under evidence/final-ed2f80a1c and final-matrix.log. No tests or qualification are attributed to the failed build.
