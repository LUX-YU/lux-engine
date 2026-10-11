# F4-C test impact and type responsibility

Entry commit: `6b777c96bbe3986c940cafe4a3e213ae2e0a3194`. Final Git objects, actual source/include/link/codegen
closure and exact I are recorded by `verify_f4.py --closeout` in the independent evidence, not guessed here.

## Actual consumers and mandatory regression

| Change | Actual dependency and verification |
| --- | --- |
| Memory.hpp/cpp allocation fact query | Foundation Buffer owner, F3 bindings and Native Frame import validation include this header; all Normal/Full ASan Foundation ownership/fault tests and GPU lifetime tests |
| GraphNative/Sync/Record/Frame/Recipes | render_vulkan_graph production objects, compiler and all Native Graph consumers; original native tests plus C1, C1-multi, C1-partial, C2, C3 and four draw fixtures |
| Executable.hpp command values | Native compiler identity comparison and all recorder consumers; independent header TU, original hot-link map, draw positive/negative admission, 2/3 slots |
| F4Closeout.cmake fixtures | Existing lux_meta_generator -> existing pass_schema.template/PassSchema.py -> emitter -> glslc -> SPIR-V validator/reflection; artifact hashes and actual codegen depfiles |
| Finalized benchmark link | Same production bind workload, original F3 binary and same-object finalized F3 control comparisons; original failures retained |

Intermediate development builds only affected targets and direct GPU cases. Final locked I requires all
146 original test names plus nine new cases (155), both Normal and Full ASan, two builds each with second
no work, plus actual closure and paired performance. No old obligation is deleted or demoted.

Unchanged Core/Transport/F2/Description/generator/Legacy/historical reports retain their Git objects.
Their original tests also run once in final integration because the stage requests the full matrix;
this does not claim their historical independent qualification was reissued. Product/Runtime/new RD2 storage,
Android/Linux and installed SDK qualification remain NOT_RUN. Three public install header prefixes are
synchronized with byte hashes solely to protect existing meta tooling; no Android build claim.

## Value and algorithm inventory

| New/extended type | Category and single responsibility | Lifetime and cost |
| --- | --- | --- |
| BufferMemoryBinding | Value: existing native allocation memory/offset/size | Snapshot only, no ownership; existing VMA allocation remains sole authority |
| VertexStream | Value: pipeline binding number selects one validated Graph vertex use plus relative offset | Cold copied into existing plan command; no resource ID or duplicate stride |
| IndexStream | Value: validated index use, UINT16/UINT32 interpretation and relative offset | No backing ownership; caller bounds actual index contents |
| DrawIndexedCommand | Value: Vulkan direct indexed arguments and stream references | Cold owned by existing recipe |
| IndirectArguments | Value: Graph INDIRECT use and command storage interpretation | One command only; no MultiDraw/count scheduling; GPU content producer must obey declared ranges and firstInstance=0 |
| DrawIndirectCommand / DrawIndexedIndirectCommand | Values: select native draw opcode and relevant stream/argument references | No new execution owner; existing queue receipt controls retirement |
| DrawCommand.streams | Existing direct command now references optional validated vertex streams | Empty remains procedural draw; no new hierarchy |
| UseRecipe.host_readback_cells | Existing private recipe gains proven output cells | Cold precompute; warmed record does not search output declarations |
| NativePassRecipe.buffer_alignments | Existing private recipe gains native use alignment constraints | Cold derive from pipeline attributes/index/arguments; invocation validates actual import offsets before GPU recording |

`validateDraw` is a cold free algorithm within existing Recipes.cpp. Native offsets are relative to the
Graph-declared BufferRange; actual backing and dynamic offsets resolve from existing FrameGraphBindings.
The closed command variant selects the Vulkan opcode; it is not business geometry dispatch. No new RAII
owner, resource list, registry, virtual interface or feature framework is created.

Native vertex footprint validation explicitly supports listed 8/16/32-bit scalar/vector and packed formats;
unknown footprints return UNSUPPORTED, not an inferred device limitation. Pipeline's existing feature/format
checks remain authoritative. UINT8 indices, >1 indirect command and IndirectCount are not enabled.
CPU admission can validate sizes/alignment/known direct counts; it cannot prove GPU-generated argument/index
contents without violating the no-readback chain. The producing shader/index author owns that bounded-content
contract; real fixtures generate only legal counts/indexes and zero firstInstance.

## New evidence obligations

- C1: real compute storage write, single/multiple queue, partial export, not-ready/BUSY, byte oracle;
  expose actual memory flags. If device has no noncoherent host-visible type, forced noncoherent remains NOT_RUN.
- C2: distinct aliased images, same handles, disjoint allocations, in-flight overlapping alias even with ready ticket.
- C3: destroy heap owner A while a completed old slot still contains its borrow; continue B and destroy plan.
- Draw: generated vertex/index/indirect schemas and actual pixels against a full-screen triangle reference;
  GPU compute creates ordinary and indexed arguments, no CPU argument readback; separate transfer/compute/graphics
  families and Sync2 trace. Wrong kind/use, offset/range, index type/count, stride, device limit, dynamic offset
  and native usage rejected before unsafe GPU access. Both two and three slots, 24 measured warmed submissions
  each, first-party C++ allocations/bytes=0 and cold/hot nanosecond statistics.

Old baseline results are not replaced by new development passes. Only the independent receipt decides final
PASS/PARTIAL/FAIL. Deferred extent/Cube/3D/IndirectCount responsibility remains in F4C_SCOPE_AUDIT.md.
