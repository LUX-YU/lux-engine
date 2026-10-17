# N3 production Texture upload through generic completion

SetTexture and SetTextureRegion now reserve the existing generic Reply slot and expected response
size before creating any Native candidate. They reborrow the moved Request from that stable slot,
then connect the existing Transfer/GpuWork through a finite Native sender. No source payload copy,
second request owner, upload state machine, callback queue or thread was added. The original
Copy/Acquire/publication algorithms remain authoritative. Their obsolete generic GPU classification
was removed; actual GPU behavior remains asserted by production and instrumented tests.

The finite sender borrows the existing completion landing place and stop token through the optional
async receiver environment. Generic Runtime adoption no longer replaces every terminal with Cancelled
on stop: actual operation semantics decide whether cancellation is proven. Accepted GPU work stopped
or lost remains OUTCOME_UNKNOWN and retains its source until Native work is safe. Synchronous commands
remain outside Pending/Sender. Canonical response code pin and selected Handler implementation pin
are distinct stable borrows; async host work retains the selected provider even after current changes.

The new production SetTexture stop/loss scenarios deliberately pause the SECOND CPU chunk preparation,
assert a prior real Submit, then stop or inject DeviceLost at the next Submit. They check returned original
source address/full capacity, retained budget, late completion after Server destruction, exact release,
and zero Validation errors. Existing Probe/GPU tests are retained, not substituted for these scenarios.
The first focused attempt paused the first preparation and failed its submitted-work assertion; that
failed evidence is preserved. The controlled pause was corrected without weakening the assertion.

Full regression initially found an actual cold registration defect: stripping the old Native rebinder
could leave a null-Device callable accepted after removing Texture's GPU enum. Typed null-object bindings
now carry no callable; only the actual Native rebinder qualifies that intermediate declaration. The
original rejection assertion remains. This is a safety correction while the remaining old registration
API is migrated; it is not the final one-call installer or permission to keep placeholder bindings.

Windows RelWithDebInfo full all -j 4 -- -k 0, second no-work, and corrected 106/106 tests passed.
Total Test time (real) = 178.78 sec
The initial 105/106 failure and both focused attempts remain under evidence/N3/native-upload;
corrected logs and raw CSV are under evidence/N3/native-upload-corrected. Durations are not a performance
comparison. Legacy719 and all three protected user files/dependencies remain unchanged. Independent
ASan verification will use the exact implementation commit and no verification-source edits.

N2/N3 remain IN_PROGRESS. Raw Submit consumers, Readback, old GPU bridge/classification/registration,
Pending variant and the remaining joint-gate contracts are not yet complete. N4-N7 remain NOT_STARTED.
Performance PARTIAL and upper product NOT_RUN/BLOCKED conclusions are unchanged. No push is claimed.
