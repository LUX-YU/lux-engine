# MA06 native invocation ownership verification

Implementation: `dcb6774850ae973fb0c499cdd38484a048f79978`. This receipt changes no production code.
Branch: `codex/editor-framework-v2`; development remains in `E:/SyncForder/CodeRepos/lux-engine`.
lux-cxx source/dependency revision: `0a0e7419fc7229df6e372cd35a540249f92250ef`.

## Scope and ownership

The existing NativeFuncCall now holds an immutable NativeCallDefinition. The definition owns
signature strings, parameter/result/receiver type descriptions, and record identity/ancestry/value
operations required by the existing compatibility and default-value paths. It is not a reflection
registry: field/method discovery remains in the original metadata provider. Its explicit CodeLease
is retained outside the definition/control block by the existing stable Object provider.

The original pin reconstruction algorithm remains unique. Rebind retains the old definition until
its pins and default values have been destroyed; node and execution-pin identity remain unchanged.
FlowSource reconstruction and the reflection palette both use the same owning input contract.
Deleted: four borrowed-reflection constructors, two borrowed rebind overloads, is_method_,
invokable_info, self_type_, and the redundant NativeFuncCall::setName forwarding body. The compiler
lowering, graph identity allocator, file format, execution and reflection registry are unchanged.

During review, simply clearing RefType::ptr was found to remove inherited-record compatibility.
The final implementation retains owned type-operation/ancestry projections, and an actual graph
connection test proves derived-to-base acceptance and reverse rejection after original metadata dies.
No claim is made that arbitrary plugin-defined Node subclasses are now safely unloaded; that belongs
to the remaining graph/payload migration.

## Actual evidence

The old installed SDK at `f3f1d4b4d0d4bb1fa5d9673317aee864443139e1` compiled and ran
`before/before.cpp`. Mutating still-live input changed the node signature to `##############`;
it returned 42. This demonstrates the borrowed-versus-owned contract gap without dereferencing
freed memory or inventing an old DLL crash. The fixture, configuration and output are retained.

The final implementation was checked out independently at `D:/LuxQualification/ma-source` and
passed ValidateTrackedSnapshot. Existing Editor/PLAYER build trees were incrementally reconfigured
from that clean tracked source; this is **not a cold-build claim**.

| Verification | Result |
| --- | --- |
| Full Editor all, second no-work, CTest | PASS, 145/145 |
| PLAYER all, second no-work, CTest | PASS, 79/79 |
| Fresh SDK native ownership, real DLL, actual AOT consumers | PASS, 3/3 |
| Standalone public headers, C++20 | PASS, 3 |
| Actual source/provider/include/link closure | PASS, 693/634/7 translation units |
| Three required module include prefixes | Exact match, 3 affected headers |
| Six protected user changes | Hashes unchanged, uncommitted |

New behavior checks cover source-string and parameter-storage destruction, pin reconstruction,
method receiver/rebind cleanup, reflection-palette lifetime, exact FlowSource capture/materialize
roundtrip, record inheritance, and invalid definition rejection. The real DLL creates a definition
from temporary strings; a host-created node invokes its callback after the external library owner
is released. The last node release unloads code only after definition/deleter/control-block cleanup.
The installed compiler builds real direct-C-ABI and reflected-invoker calls after signature source
destruction. This is AOT object generation, not execution of those two compiled entry points.

All previous test names remain; Editor adds three tests and PLAYER adds two. No old assertions were
removed. Flow's actual dependency closure contains no Engine, Editor, UI or PluginManager provider;
the installed consumer uses installed headers/libraries, not source-private headers or build DLLs.

## Archive and limitations

Evidence: `E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma06/native/evidence`.
Manifest SHA256: `05856fc7c01ac14865e976871ef1915d29da20d471f769b15eacd976b6293011` (1058 files, 25 commands).
The archive was verified after relocation to a Chinese/space path. Missing and tampered actual SDK
CTest evidence were both rejected; restored evidence passes. Required paths are archive-relative.

**Only this native-signature closure passes. MA06, MA08 and the overall task remain incomplete.**
FlowNodeCatalog, the real public compile extension role, and removal of legacy Node/Pin structural
authority remain pending. Native metadata ownership does not substitute for those requirements.

The original SDK desktop suite was not rerun: its 14/15 failure at `617403987b91940b4851b2d8007755a96e492494`
is inherited unchanged, including Q-LR03-HOST-MINIMIZE and the unproven common cause of phase-8 failures.
Linux remains NOT_RUN/unmet; LR08 remains PARTIAL. Native input remains NOT_RUN_USER_DEFERRED;
IME, historical skinned WAR and other deferred qualifications retain their original scope.
No MA sanitizer result is claimed. Android headers were synchronized only, not built or tested.
Original ProjectBuilder user patch remains externally preserved and unapplied. Main and historical
snapshots are untouched. Continue the authorized remaining MA work without requesting another stage approval.
