# F6.b source/consumer impact before implementation

Base: c6400f4d97af453da1fa2815356c9fea477458ab; comparison I: bf3223038d415ed6912dcea04dc8f4d195becd3e.

The user supplied the complete F6.b work order and authorized continuation after the preflight archive incident.
The original F6.a manifest is retained. Exactly one historical compiler-intermediate vc140.pdb differs;
all other 3785 recorded files, including final EXE/PDB/libraries, reports and performance samples match.
This exception is not a performance waiver or a change to historical qualification.
Evidence: D:/LuxQualification/render-v2-f6b/preflight/historical-archive-audit.json.
All new commands must isolate compiler/linker intermediates as well as final outputs.

## Reproduced prerequisites

- Installed Data SDK: two render_data_schema calls on one target (a/Types.hpp, b/Types.hpp)
  fail with duplicate job probe_data. Work-order 8.1 permits a bounded job/output identity repair.
- Real unchanged F6.a native libraries: two separate SCENE shapes used by complementary shaders
  produce valid Shader/Pipeline candidates, then Invocation construction rejects with
  kInvalidArgument {2,0,3}. Recipes.cpp selects the first category-matching owner.
  Probe: preflight/native-owner/result.json and run.log. This proves cold assembly rejection,
  not an executed GPU hazard. The initial incorrectly selected harness run is retained separately.

## Minimum production impact

| Source | Consumer / change | Qualification |
|---|---|---|
| shader/Layout.hpp/.cpp | replace three-category assignment with explicit generated field-path to structural owner associations; full shapes remain cold owned values | Layout/device budgets/full-shape/collision tests |
| shader/Program.hpp/.cpp | accept assignments; retain relocated field ownership and genuine Data ABI checks | original Shader/relocation/last-good tests |
| shader/Bindings.cpp | validate the exact compiled field, not any same-category/semantic field | descriptor negative/foreign device/in-flight tests |
| graph/Recipes.cpp | necessary narrow lower-layer change proven above: consume compiled field-owner association for cold full-shape source resolution and descriptor assembly | NativeGraphShared + new multi-owner Invocation oracle |
| Description PassContract, DataLayout and offline Shader validation | synchronous generated GPU contract and complete cold identity; actual SPIR-V validation | generator/reflection/layout mismatches |
| Graph codegen only | project existing parser Data annotations through shared GPU value generation; no F2 C++ or scheduling change | original 197 obligations plus generated multi-owner fixtures |
| Data codegen CMake | only the reproduced job/output collision | two equal basenames on one target and installed consumer |

No Runtime, Queue, Foundation, Retirement, Record or Sync2 algorithm change. No new resource owner,
registry, thread or second layout compiler. Native recipe adjustment is a cold association fix,
not a permission to rewrite C11/C12. Full ABI/source migration is required for test authors using
OwnerAssignment; there will be no production three-slot compatibility adapter.

F6.c DataSet, F6.d SDK, F7 storage and product integration are not implemented by this stage.
