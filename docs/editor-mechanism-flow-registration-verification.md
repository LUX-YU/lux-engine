# Flow registered pure-value compilation

Implementation: `a6d4242c101f8ecbe3d83123037c36b376646d80` (initial slice
`67625e41fb4c1d9f0f26a9f1358223c2002c1088`). lux-cxx:
`0a0e7419fc7229df6e372cd35a540249f92250ef`.
**This pure-value compilation slice passes. MA06, MA08 and the complete migration remain incomplete.**

## Responsibility and deletion

Flow owns immutable canonical node definitions, type/version admission, plain payloads,
semantic pin declarations and actual registered compile callbacks. The example extension
compiles a two-output polynomial through separate scalar primitives; the host contains no
polynomial type branch. FlowValueCompiler exposes ephemeral typed values, not MLIR or graph
ownership. The original builtin scalar emitter and registered callback adapter share one
backend primitive implementation. EScalarInstruction moved out of the private selector;
no old namespace alias or second opcode emission algorithm remains.

GraphTopology still issues identities and links. The current RegisteredValueNode is an explicit
private transition adapter, not a claim that the remaining polymorphic Flow store has been
replaced. Definitions and payloads retain their code lease; schema admission permits editable
non-compilable drafts, and compilation retains the original owning failure. Registered source
nodes are not silently serialized as successful empty nodes: full canonical Flow codec migration,
control/native/Ability registrations, plain graph stores, Node.graph removal and graph UI remain.

## DLL lifetime finding

A real installed-SDK DLL constructed a catalog and returned its shared definition. Once the
catalog and external library owner were gone, releasing the last definition crashed with
0xC0000005 on the initial implementation. The correction uses the existing Object provider's
pinCodeOwner bridge around the foreign definition control block. It covers destruction and
return before releasing plugin code; no new lifetime manager was introduced.

The initial standalone fixture omitted a used host import of the Object provider. At both
initial and corrected SDKs it crashed; the corrected crash's debugger trace shows that unloading
the plugin also unloaded lux_engine_core_object while its release bridge was executing. These
outputs are retained, not relabeled PASS. The corrected fixture follows the existing host-first
ObjectRuntime contract. With that **identical** host setup, the old SDK still crashes inside
unloaded definition.dll while Object remains loaded, and the corrected SDK returns successfully
and confirms library expiration. A plugin's dependency load alone is not a host lifetime anchor.
The qualification does not promise safe dynamic unloading of the foundational Object provider.

Three separate real graph/compiler tests exercise a host contribution, a DLL contribution, and
a DLL-created catalog definition after catalog release. Each compiles, links and executes 77
polynomial invocations, then verifies unload. The standalone final-owner probe is separate from
those graph tests. Existing MaterialNodeCatalog showed a superficially similar source pattern;
its actual SHARED provider boundary must be qualified separately, not called fixed by this change.

## Actual qualification

Independent clean tracked checkout; reused Editor/PLAYER build trees (incremental, not cold).
Every build uses `--target all -j 4 -- -k 0`, followed by no-work verification. Fresh SDK prefix
uses installed headers and libraries only.

| Scope | Result |
|---|---|
| Editor full CTest | 161/161; all 157 preceding names retained |
| PLAYER full CTest | 89/89; all 88 preceding names retained |
| Installed analysis/control | 2/2, standalone headers |
| Installed native/compiler/DLL | 6/6, standalone headers |
| Installed scalar/control/nested/registered | 7/7, standalone headers |
| Installed graph/domain/compiler | 4/4 |
| Installed Flow payload/DLL | 2/2 |
| Installed Ability/DLL | 3/3 |
| Registered actual native execution | 3 x 77 invocations |
| Nested control actual native execution | 48 invocations |
| Host-initialized foreign definition release | Initial SDK AV; corrected SDK exit 0 |

Twelve full semantic diagnostics are byte-identical through module and compiler. The scalar AOT
output retains 123 exports / 28393 bytes; three async outputs retain 4931 framed bytes; six legal
control outputs retain 5772 raw bytes. Invalid outside-loop break preserves its exact failure.
Byte parity describes code generation, not execution. The native invocation rows are actual execution.

Pin schema, invalid callback output, code-owner mismatch, compile rejection, codec callback failure,
and canonical graph identity assertions remain. The first new test mistakenly omitted the existing
`compile failed:` diagnostic prefix; its failures are retained and the test now checks the full error.
Provider/include/link checks confirm Flow has no Editor/UI/Toolchain reverse dependency. Four changed
module headers match Debug, RelWithDebInfo and Android include prefixes; Android was not built.
All six protected user-file hashes match; ProjectBuilder's archived patch remains unapplied.

## Evidence and remaining scope

Archive: `E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma06/flow-registration/evidence`.
1259 files / 105 command records. Manifest SHA256:
`be39279b5075868bc4e3d395d737a93cbb5c6e6293697ce8fca100fa14e78d81`.
Relocation to a Chinese/space path, missing-output rejection and tamper rejection passed.
The initial SDK, both failed standalone runs, debugger output and corrected-host comparison are included.
SDK selection in the before/after comparison is recorded independently of the runner's source checkout SHA.

Linux remains unmet / LR08 PARTIAL. Native input remains user-deferred; IME and sanitizer scope
are unchanged. Host minimize, skinned WAR and VERTEX_COLOR findings remain OPEN. No new GPU/input
qualification is claimed. Historical receipts are unchanged; this is not overall MA completion.
