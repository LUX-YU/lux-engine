# Flow graph receiver ownership correction

Implementation: `bcbcf722c98334ecb1cbddd49b56c3f183aef90a`. This corrects a real graph mutation boundary; it does not finish MA06/MA08.

## Failure and correction

The real installed SDK at `5f59192e21d5e177f728d75ed2bde5dd56d7a01b` reproduced both cases:
two graphs had equal local NodeId/PinId values, and calling the target graph's `connect` or `disconnect`
with the other graph's Pins modified the target graph. Both executions returned 42, with the foreign
graph unchanged. The fixture restored and compared both complete source encodings before cleanup.
These are executed SDK failures, not the source-only concern in the previous receipt.

The original two entrypoints now reject Pins whose node is not attached to that receiver. Rejection
uses the existing INVALID_PIN result and occurs before virtual link preflight or topology mutation.
No new IDs, counter, registry, storage, wrapper, helper protocol or graph edit algorithm was introduced.
The guard is established for each public call; it is not a cached cross-callback validity claim.

The existing structure regression now checks both foreign Pins, either mixed source, and moved graph
ownership. Rejected operations preserve both graphs' complete encodings. Valid same-graph connect and
disconnect still succeed. All earlier identity, dynamic-pin, source, move and edit-replay assertions remain.

## Fixed-commit qualification

`ValidateTrackedSnapshot` and an independent clean tracked checkout were used. Editor and PLAYER build
trees were reused; this is incremental qualification. Full `--target all -j 4 -- -k 0` and second
no-work builds passed. No build ran concurrently with GPU tests.

| Actual run | Result |
|---|---|
| Editor | 152/152; previous test names retained |
| PLAYER | 86/86; previous test names retained |
| Installed Native/DLL/compiler/structure | 4/4 |
| Installed scalar AOT | 1/1 |
| Installed shared graph/domain/compiler | 4/4 |
| Installed erased payload/DLL | 2/2 |
| Installed Ability metadata/DLL | 3/3 |
| Public standalone headers | 10 |
| Original unchanged before fixture against new SDK | connect and disconnect both return 0; receiver unchanged |

The real AOT run generated 123 scalar exports, 28393 bytes. The output is identical to
the qualified scalar result at `f3f1d4b4d0d4bb1fa5d9673317aee864443139e1`:
`dbca5b42fd333d757779b33a84f365a8a0141fda677cdf35169cf56fc3b2516c`. The archived old result was compared, not represented as a new old-SDK run.

Actual source/provider and include/link inspection confirms Flow has no upward Engine/Editor/UI/Toolchain
dependency. Consumers use the fresh install prefix, without source/private header or build-DLL fallback.
No public header changed in this correction. The previous eight structural compiler negatives belong
to implementation 5f59192e2 and were not rerun or claimed as new results here.

## Archive and limitations

Evidence: `E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma08/flow-receiver/evidence`.
1137 files / 42 commands; manifest SHA256
`05371fa54c9e6d6f311fca13877fb4fdf35b39633dd3c1392d0f36a948b4efd7`. Chinese/space path relocation verified. Missing and tampered actual SDK
test logs were rejected; restoring the original bytes passed verification.

Six protected user differences are unchanged, the separately archived ProjectBuilder patch remains
unapplied, and main/history are untouched. Original LR08 PARTIAL/Linux unmet, deferred native input,
IME and prior sanitizer scope remain. The historical installed-host minimize failure, skinned WAR and
VERTEX_COLOR compilation failure keep their earlier OPEN status.

Flow's polymorphic Node/Pin identity fields and linear lookup remain. The registered domain catalog,
payload store, compiler extension migration and graph UI qualification still require implementation;
neither this correction nor the prior visibility restriction closes those final authority requirements.
