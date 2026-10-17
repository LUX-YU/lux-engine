# Typed Feature checkpoint: failed verification and admission-fixture repair

76b9c1189dd80bc0ce09f5bcac4edd9a74b598b5 independent ASan verification was 105/106, not PASS.
render.runtime.gpu_ready timed out at line 466 waiting only for samples[7].submit. The original log
contains no terminal Reply observation, so the exact original refusal cannot be reconstructed.
An unchanged isolated rerun passed (8.85 s); this does not erase the failed run.

The fixture had an independently reproducible missing branch: successful IPC submission does not
promise Native admission. A pre-submit Queue capacity failure leaves submit==0 permanently and returns
a valid FAILED Reply. The accepted-work helper already used by abandon/shutdown now also covers the
four-View held-upload scenario. It consumes only explicit kCapacity failures, asserts zero GPU submit,
and retries within the existing 15 s deadline. Other errors and timeouts remain failures. A deterministic
queue-saturation prelude proves this case on each topology; typed recovery checks the original source
address before releasing the failed completion. Capacity, rendering-progress and GPU success assertions
are unchanged. No production Queue/IPC/ownership code changed.

Implementation: full all -j 4 -- -k 0, second no-work; all three real GPU-ready topologies passed,
22.33 s. First repair build rejected default assignment of move-only TypedRenderCompletion; fixed by
lexical scope, not by changing the public contract. Failed attempts are retained outside the source tree.
A new exact-I independent full matrix is required. N2/N3 remains IN_PROGRESS; no N4 entry or push.

Correction to N2-TYPED-FEATURE-CHECKPOINT.md: its 0.12 s was a nested consumer CTest duration.
The complete 106/106 implementation run was 138.57 s, from the final unprefixed Total Test time line
in test-RelWithDebInfo-20261017-031336-551.log. Original note remains preserved; no historical
performance qualification changes. Performance PARTIAL; product root historical BLOCKED.

In-progress ConfigValue/FeatureUse edits were saved without loss to external binary patch
feature-config-in-progress-20261017.patch, SHA-256
A3A1FD60BD4EAE84345C89CEF7E1896DD146B46CA2F7CF7BFC4D682ACFFB6D58.
They are excluded from this correction commit and will resume in the same N2/N3 gate.
