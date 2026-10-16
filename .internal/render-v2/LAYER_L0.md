# Layering L0 — frozen implementation boundary

Baseline: `7ca2828d60df50e54ed299efeb1cbab20cf369ad`. Worktree: `E:/RenderV2Rebuild/Layer/source`.
Contract SHA-256: `5d7db67994b2cd940ed04f824c18007ed61f6f815deb36756a9f41b0fb0d7f70`.

L0 is an audit checkpoint, not implementation qualification. 719 frozen blobs and the 98 existing test names were checked. Dependency commits remain lux-cxx bf779515a120350c7c5412c366eea73afdf59ccf and toolset c104eb6dd30a45f5d8e76f24e8d6f8de064bdf60. All other worktrees, including their existing dirty files, remain untouched.

## Actual owners and commit boundaries

- RenderServer::Impl owns the single Command SlotMap, storage/feature cold registrations, Scenes/Targets/Effects, one DeviceSession, and stable deferred slots. Registration precedes run; capability publication follows native installation.
- Scene owns Views. Effect owns one CompiledGraph and its four-field ExecutionGroups. FeatureAssembly is a cold candidate, not another runtime owner.
- DeviceSession owns VMA allocations, queues, Transfer, Retirement and ResourceRegistry. Registry entries exclusively own actual native storage. Factory and binding Ops are immutable borrowed code under their code pins.
- IPC owns the original typed request until returned; Runtime holds the ReceivedRequest for deferred work. CPU completion events carry only slot identity. GPU callbacks carry real queue tickets; neither callback is a second request owner.
- Compilation workers consume frozen registrations and an owned request candidate; RenderThread alone adopts successful results. Stop joins/settles work before releasing the Responder and DeviceSession.

No new long-lived owner is required. New registration qualifiers are cold values; ProgramContribution and explicit compiler spans are inputs. Preserve callback order, Queue capacity 3, sleep_for(1ms), IPC algorithms and LastUse.

## Mutation whitelist

Production/test changes: modules/function/render/{core,data,protocol,resource,graph,features,native/vulkan,runtime}/ and render component exports; necessary cmake/render-v2-bootstrap consumer configuration. A minimal existing-platform random-byte seam may change only after its exact consumers are recorded. New audit/checker material is confined to .internal/render-v2/. Legacy, old reports/checkers, platform IPC implementation, dependencies and original worktrees are read-only.

## Gate and issue matrix

| Items | Current status | Authorized action |
|---|---|---|
| L01, L07–L12, L14 | OPEN | L1 typed registration, explicit factories, Texture resource declaration, sync/async split |
| L02–L06 | OPEN | L2 Feature descriptor/assembly split, neutral Graph/Shader/Binding inputs |
| L13 | DEFERRED | E1 execution-layer move requires separate gate |
| L15–L17 | OPEN | L2/L3 target closure, public/private headers and single Impl file split |
| W01–W02 | OPEN | L3 Encoding terminology and Payload owner dependency |
| W03 | DEFERRED | W1 typed configuration representation |
| W04 | OPEN | L1 actual Config/Ops qualification; W1 representation stays deferred |
| W05 | OPEN | L0/L3 consumer classification, no format change |
| P01 | DEFERRED | P1 fixed-work scheduling investigation |
| P02–P03 | DEFERRED | P2 retirement/lookup measurement and optimization |
| P04–P05 | DEFERRED | P3 descriptor capacity measurement |
| P06 | DEFERRED | P1 fairness and tail qualification |
| E01–E02 | OPEN | L4 bounded completion and worker handoff regression |
| E03 | DEFERRED | S5 frame scheduling |
| E04 | OPEN | L3/L4 invocation and ownership boundaries |
| F01 | DEFERRED | Future S3/S4 primitives, no new production feature |
| F02 | OPEN | L2 compiler input narrowing; broader recipes deferred |
| D01 | OPEN | L0/L3 current documentation |
| D02 | PROTECTED | Root TOOLCHAIN remains independently BLOCKED unless actually repaired |
| D03 | OPEN | Platform API isolation; unrun platforms stay NOT_RUN |
| D04 | OPEN | L4 portable evidence subset plus external raw archive |
| D05 | OPEN | L0/L3 encoded-data producer inventory |

Protected regressions: C01 failed/cancelled Request recovery, C02 canonical SBO policy, C03 LastUse. Existing performance PARTIAL is unchanged.

## Encoded-data consumer classification

- Feature configuration: CreateEffect owns SharedBytes; assembly borrows synchronously; compile worker lifetime is protected by the original request. Current clients/tests produce it in-process. No new persistent asset format is inferred.
- Storage configuration: current Texture Config is a typed shared object; DataSchema describes it but does not prove C++ void* cast identity. Factory templates must supply that missing proof.
- Attach binding arguments: Effect owns bytes, Native preparation validates once and produces stable bindings; session-local TextureId makes these unsuitable as persistent cooked assets.
- Texture payload: RenderPayload retains the complete SharedBytes backing and source pin through asynchronous upload. It must not depend on the configuration codec merely to own bytes.
- Shader arrays: SPIR-V is cooked shader data, not a Command wire representation. Its interface describes GPU binding layout, not Feature authority.
- Data codec tests are format-contract consumers; retaining their names/bytes is required. No production typed Command is to be moved back to encoding.

## Evidence

External immutable-at-gate inputs: E:/RenderV2Rebuild/Layer/evidence/L0. snapshot.json records all worktree HEAD/status, user hashes and dependency versions; test-names.json records 98 names; include-edges.json records 800 edges; consumers.json records 457 matched consumer lines; cmake-inputs.json preserves actual build inputs. Baseline size facts (IPC 144B/80B SBO, Reply 304B, TypeOps 88B) and 98/98 x3 are inherited from prior independent V, not rerun L0 results. New builds must measure their own qualification.
