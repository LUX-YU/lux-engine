# Typed Feature installation and erased registration removal

N2/N3 is IN_PROGRESS. Feature installation now obtains its graph contribution and concrete Handler
from one typed provider, constructs or reuses the unique Handler by C++ type and instance key, and
uses the same qualified member/Command publication transaction as Storage. Actual Native handlers
borrow DeviceSession; CPU fixtures can pass their concrete constructor arguments. No null Device or
second binding step remains. Internal feature_detail::Implementation is synchronous template-install
material (descriptor/graph/program only), not a public erased Command/Owner aggregation.

Stable FeatureEntry owns its name, descriptor and graph callback table and source Code Pin. Worker
compilation borrows this stable entry and this request; it does not capture the mutable Impl. Dynamic
Owner-thread installation does not invalidate old IDs/addresses. Same definition is idempotent;
conflicting definitions or Command qualifications cannot publish partial Feature/support entries.
Zero-pass graph-only providers use Handler=void and no dummy Program. External Buffer Feature and
Storage share one actual BufferCommands object: repeat installation constructs once, shutdown destroys
once, while all original two Scene/four View GPU work remains.

Deleted CommandRegistration, bind_native_, bindNative(), nativeCommand/nativeFeatureBinding and the
public FeatureRegistration aggregate; resource/Registration.hpp and native/vulkan/Registration.hpp
are deleted with no forwarding headers. CommandMetadata uses the immutable typed CommandHandler;
legacy second binding/validation routine is gone. Source public-header probes and relocated SDK stale-
header assertions migrate the precise deleted files, without disabling a CTest. ProtocolAudit.hpp now
contains only deliberate malformed-type/Reply fault access; the old erased registration test adapter
is deleted. Real Runtime/GPU/Batch/async consumers use typed Handler members.

Historical duplicate-Command rejection is superseded by shared supporters, and repeated identical
Feature installation is now idempotent. Wrong Request/Reply/member pairing is a compile-time negative;
corrupt full type/name/layout and duplicate member bindings remain cold negative tests. Closed Server
rejects installation; mutation from a concurrent Client thread is not permitted or tested as valid.
600 initial Features plus late Owner-thread install survive destruction of temporary metadata; returned
names survive Server. Cross-DLL async VALUE/FAILURE/STOPPED tests retain callback-tail and exactly-once
module unload assertions; the plugin now installs its real owned Handler directly at the cold boundary.

Full all -j 4 -- -k 0, second no-work; focused 7/7 then full 106/106 (0.12 s).
First build exposed stale probes for the two deliberately deleted headers and const completion input
in a new test; fixed in implementation, failures retained. Installed/relocated consumer checks and GPU
regressions included. No production IPC, Queue, Transfer or retirement algorithm modification.
Independent V pending for this I. FeatureId/typed ConfigValue selection, removing manual Feature version
and narrowing Hello bootstrap remain before the final joint N2/N3 gate. N4-N7 NOT_STARTED. Performance
PARTIAL, root product historical BLOCKED, upper products and unrun platforms NOT_RUN. No push.
