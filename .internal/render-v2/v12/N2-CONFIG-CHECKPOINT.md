# N2 configuration prerequisite (not the joint N2/N3 gate)

ConfigValue shares one immutable C++ configuration and its source-module code pin. It has no mutable
cast, clone, IPC storage policy, serializer or owner directory. Full TypeToken, sizeof and alignof
are retained as static metadata; cold Factory checks preserve all three. The pin outlives both the
object/control-block deleter and its metadata. Assignment exchanges both owners before destroying
the previous value. Dynamic configurations must declare full retained backing/capacity; ordinary
allocation exhaustion follows the existing fatal convention.

Storage registration and the existing Registry configuration now use this same value instead of
independent shared_ptr<void> plus caller-supplied type metadata. Actual Storage ownership is not
changed here: Domain/Factory ownership moves in N4. FeatureUse and typed Graph configuration remain
to migrate during N2/N5. This checkpoint does not declare those work packages complete.

The new test verifies rejected unaccounted dynamic data, byte undercount rejection, wrong C++ type,
shared pointer identity, moved-from state, full 4096-byte Asset backing behind a 2-byte subrange,
exact destruction, over-aligned vector configuration crossing an actual DLL, and final object
destruction before DLL unload. Public and relocated-installed consumers compile the new header
independently, with no RTTI or Native leakage. The installed executable also consumes a shared value.

Implementation RelWithDebInfo: full all -j 4 -- -k 0 and second no-work passed; 105/105 tests passed
in 112.72 seconds. The first full attempt found the installed Data test still expected five error
descriptors; its exact assertion now expects the newly added sixth descriptor, with original checks
retained. Earlier compilation errors and the failed installed run remain in the external evidence.
Evidence directories: E:/RenderV2Rebuild/V12/evidence/N2/configuration*, with console logs also at
E:/RenderV2Rebuild/V12/config-{build,no-work,full-test}.log. Independent exact-commit ASan validation
must follow this checkpoint before using the configuration contract in the next ownership changes.

No IPC, Transfer, Graph analysis or GPU synchronization algorithm changed. Legacy 719 and all three
protected user files pass. No performance conclusion or push is made. N2/N3 remains IN_PROGRESS.
