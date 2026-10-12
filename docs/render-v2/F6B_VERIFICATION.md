# F6.b independent verification — PARTIAL

Qualification date: 2026-10-12. This receipt was produced from the exact clean implementation clone.
Functional, lifetime, generated ABI, installed binding consumer, Normal and Full ASan qualification pass.
**F6.b overall remains PARTIAL:** new Record/Submit p95 uncertainty and a demonstrated cold Layout cost
increase are not waived. This is not F6.c authorization or completion of F6 overall.

## 1. Immutable Git identity and independent execution

- Approved remote/base: `c6400f4d97af453da1fa2815356c9fea477458ab`.
- Initial implementation: `fb229ba44fa8211c19e1eefd2717f0f7c3f0e972`, parent approved base.
- **Final Implementation I: `7406965112f09807b9f70aa3e739ff43539969c8`**.
- I's actual parent: `fb229ba44fa8211c19e1eefd2717f0f7c3f0e972`.
- Final child changes only MultiOwner test completeness, closure-check path memoization and implementation
  documentation; no production change after the initial I. Prior independent candidate results remain archived.
- Verification V is the report-only child of I containing this file. Its exact SHA is obtained after commit
  and recorded in external delivery.json and the user delivery; it is not self-referenced in its own Git content.
- Aggregate implementation: 52 changed files, enumerated in [F6B_IMPLEMENTATION](F6B_IMPLEMENTATION.md)
  and exact blobs in final-audit.json. V must add only this report.

Clone command uses `--no-hardlinks --no-checkout`, detached exact I, fixed Git newline conversion.
`isolation.json`: clean=true, alternates=false, maximum object link count=1. Tracked snapshot check passes.
Source: `D:/LuxQualification/render-v2-f6b/qualification/7406965112f09807b9f70aa3e739ff43539969c8/source`. Normal/ASan build, installation, external consumers and evidence are siblings,
never patches inside the validation source. No independent-clone production repair was made.

## 2. What changed and what owns it

The three category assignment members are removed. `OwnerAssignment` now maps one generated field path
explicitly to a structural ShaderOwnerId. Category remains scope classification, not identity or selection.
`shaderOwnerId` incorporates canonical semantic slot, revision, category and full sorted field shape,
including generated Data ABI; full equality remains necessary after hash. Complete unused fields count.
`lighting.main` and `lighting.preview` can use the same LightGpu type without conflating runtime instances.
Scene identity, DataSetId, Buffer handle, revision of instance data and FrameSerial do not enter this key.

New public value: `PassDataContract`; changed value: `OwnerAssignment`. Existing OwnerField and
DescriptorLocation gain owned cold ABI bytes/path. No new public class, native owner, persistent registry,
thread or scheduler. See [type inventory](F6B_TYPE_INVENTORY.md). Four structural owners occupy two physical
sets in the new fixture; Set placement remains the existing compileLayout algorithm.

Actual chain: DataSchema -> PassDataContract -> real SPIR-V field/stride validation -> compileLayout ->
relocation/NativeShaderProgram -> existing Native recipes -> fixed descriptor binding and Invocation.
A narrowly scoped cold Recipes.cpp consumer fix uses the compiled field owner; preflight proved rejection
with two same-category owners before that fix. No Record/Sync2/Queue/Retirement or F2 scheduling change.
See [source/consumer/ABI impact](F6B_NATIVE_IMPACT.md), including codegen identity repair.

## 3. Builds, tests and installed closure

| Qualification | Normal | Full ASan |
|---|---|---|
| Complete bootstrap `all -j 4 -- -k 0` | PASS | PASS |
| Second complete build | `ninja: no work to do` | `ninja: no work to do` |
| CTest | 200/200, zero failed/skipped | 200/200, zero failed/skipped |
| Original 197 test names and obligations | retained and executed | retained and executed |
| Installed Data consumer (own generated types) | configure/build/run PASS | configure/build/run PASS |
| Installed RenderBinding consumer | configure/build/run PASS | configure/build/run PASS |
| Actual source/include/link/codegen closure | PASS | PASS |

New test names: `render.vulkan.multi_owner`, `render.data.codegen_identity`, `render.data.reject_nested`.
Original Reflection/Serialization projection, Graph oracle, Shader/relocation, native lifetime and Runtime/WSI
obligations actually ran. This report does not replace execution with historical 197/197 claims.

C++20 MSVC 19.44.35228, RelWithDebInfo, Ninja; ASan `/EHsc /fsanitize=address /Zi` and compatible installed
SPIRV-Cross/lux-cxx dependencies. Commands, return codes and compiler inputs are archived.
Each closure has 521 File API targets, 66 codegen inputs and six directly changed header checks, plus
existing standalone-header/Concept tests. Ninja deps/commands and linked inputs show no private/source-tree
fallback in the external consumers. Data remains Core/neutral metadata only; no Vulkan/Runtime dependency.
82/82 comparable original SPIR-V artifacts remain byte-identical in each mode, including runtime-generated
artifacts. 67 was only the earlier development-build inventory, not a relaxed final coverage count.

Two equal-basename author headers are generated in one installed consumer with distinct OUTPUT_NAME/job
identities. Unmarked nested GPU record rejection uses the real generator. Actual Shader reflection validates
Data ABI; `validated=true` alone is never treated as arbitrary Shader compatibility.
Installed RenderBinding proves the structural binding value API against installed libraries; it does not
claim an installed offline Shader compiler, plugin SDK or external plugin GPU execution (F6.d).
Public header Debug/RelWithDebInfo/Android prefix synchronization is hash checked; not Android qualification.

## 4. Real GPU, scope, lifetime and rejection evidence

Device: NVIDIA GeForce RTX 4070 Ti, driver 591.86, Vulkan API 1.4.325; SDK 1.4.304.0.
VK_LAYER_KHRONOS_validation and synchronization validation are enabled by the actual fixtures.
**Validation error count = 0** in both modes. GalaxyOverlay layer naming warnings remain in raw logs;
zero errors does not mean zero warning output. Original native and Win32 presentation tests also executed.

G1/G2: generated LightGpu main/preview plus generated MaterialGpu, all same SCENE category, one compute
Shader, independent native inputs. Material CPU size differs from GPU stride and is explicitly packed.
G3/G4: eight configurations (1/2 Scene identities x four Views x 2/3 FIF x View work/Scene donor reuse).
Each configuration shares one actual Pipeline/PipelineLayout/native code, with 8/12 independent slots.
Different Scene backing yields different correct values. Batch 2 swaps main/preview and changes only Scene A
Material roughness; Scene B stays unchanged. Donor reuse counts equal the exact permitted counts.

Each mode produces 240 four-float readbacks (960 independently checked components). A separate evidence
reader decodes the recorded hex and recomputes all values: PASS. CTest merges stdout/stderr; derived extraction
only strips exact overlay warning lines that interrupt a readback line, preserving the untouched LastTest.log.
JUnit truncates large stdout; full LastTest logs, not truncated JUnit text, are the raw output authority.

Inputs retain actual accepted queue tickets. Writes/rebinding occur after proven completion. Donor readback
waits for all accepted consumer borrows, not just donor receipt. Warm fixed execution allocation count AND
bytes remain zero. No lazy Shader/PSO/Layout creation. CPU Scene identities/native fixtures here do not
constitute Scope-owned DataSet/F6.c; original F5 Runtime multi-Scene/View/Target tests run separately.

G5/G6: owner/assignment missing, duplicate, unknown/conflict; complete unused shape and forced hash mismatch;
Data ABI/schema version/reflection mismatch; descriptor count/type/usage and device budget/indexing flags;
actual multi-owner missing/extra/duplicate/foreign-device values; existing inflight descriptor rewrite,
foreign ticket, alias, last-good, R4/F5 readiness/retirement/WSI cases all retain executable rejection oracles.
Errors are rejected before illegal GPU use. Test-author-only fixtureOwners migrates old F3 inputs; there is
no installed production compatibility adapter. No old numerical oracle or Shader work is reduced.

## 5. Frozen performance experiment and hot result

A = F6.a I `bf3223038d415ed6912dcea04dc8f4d195becd3e`; B = final I above.
Original real ShaderBenchmark and NativeGraphBenchmark operations/Shader bytes/timing boundaries preserved.
Protocol and script frozen before measurement, 8 AB + 3 AA + 3 BB pairs per workload, **56/56 independent
processes, all exit 0**, no replacements/exclusions/additional pairs. 392 metric records, 54,600 raw timing
samples (fixed Binding batches contain 1000 actual operations each). All warm allocation counters are zero.
Seed 20261012, logical CPU 2 affinity mask 4, AboveNormal, high-performance power plan, QPC 10 MHz.
CPU i7-13700KF (16 cores/24 logical), Windows 11 Pro build 26200. No concurrent build/GPU/profiler run.
Process CPU time/I/O/working-set availability is retained per command; unavailable metrics are not zeros.

Protocol SHA256: `8baecfa71b974a3433c48f2c39d16ab453f95d380166b953b5d00473ec59ee4d`.
Script SHA256: `8a280096307be602723259818affcbd476c8dbcb615a40a4481a0ccd81a06a15`.
83 input/binary/PDB/Shader identities were frozen. A executable hashes match its retained archive manifest.
CI95 uses 10,000 fixed-seed bootstrap resamples of process-paired ratios; no pooled-frame substitution.

| Endpoint | Paired p50 change / CI95 | Paired p95 change / CI95 |
|---|---|---|
| `fixed_native_binding_recipe` | -1.6949% [-3.3333%, +0.0000%] | +0.4000% [+0.0000%, +3.2520%] |
| `native_graph_record_submit_256x256` | +0.2241% [-7.6923%, +3.3333%] | +5.0936% [-13.7143%, +17.8182%] |

| Endpoint / control | p50 change / CI95 | p95 change / CI95 |
|---|---|---|
| fixed_native_binding_recipe / AA | +0.0000% [-1.6807%, +1.7094%] | +0.0000% [-34.0426%, +1.6260%] |
| fixed_native_binding_recipe / BB | +0.0000% [-0.8547%, +0.8621%] | +0.0000% [-0.8000%, +3.2787%] |
| native_graph_record_submit_256x256 / AA | +0.4367% [-0.8032%, +6.4378%] | +7.4074% [-8.1967%, +15.4676%] |
| native_graph_record_submit_256x256 / BB | -1.3158% [-9.0909%, +3.0973%] | -7.9618% [-27.5132%, +17.2414%] |

Fixed Binding: PASS. Record/Submit: **PARTIAL**, p95 CI upper +17.8182% and unresolved AA/BB p95 controls.
This does not establish a +5.0936% causal production regression, nor exclude one. No attribution to Windows,
driver or scheduling without evidence. This new F6.b uncertainty is separate from approved historical debt.
No repeated experiment was run to seek a favorable interval.

## 6. Cold results: an additional unresolved new cost

| Endpoint | Paired p50 change / CI95 | Paired p95 change / CI95 |
|---|---|---|
| `layout_full_shape` | +183.3333% [+183.3333%, +183.3333%] | +137.5000% [+125.0000%, +171.4286%] |
| `relocation_validation_vertex` | -1.3718% [-2.6524%, -0.6412%] | +0.9991% [-4.7736%, +5.2300%] |
| `complete_descriptor_pool_sets` | +0.0000% [+0.0000%, +0.0000%] | +0.0000% [+0.0000%, +0.0000%] |
| `native_graphics_pipeline_create_destroy` | +0.0000% [+0.0000%, +0.0000%] | +0.0000% [+0.0000%, +0.0000%] |
| `C9_identity_and_candidate` | -3.1703% [-6.1224%, +2.1739%] | +3.5468% [-5.0847%, +13.4615%] |
| `C10_native_resources` | -2.7973% [-4.1958%, -1.4286%] | -22.3592% [-29.6117%, +8.8007%] |
| `C11_fixed_recipes_and_descriptors` | +3.7110% [+2.0513%, +7.0352%] | +4.6007% [-4.8872%, +12.4498%] |
| `C12_C13_sync_queue_admission` | +1.9231% [+0.0000%, +3.8462%] | +1.7857% [-3.4483%, +28.5714%] |
| `native_graph_compile_prebuilt_programs` | +0.2346% [-0.9412%, +2.3810%] | -1.0328% [-11.8893%, +18.3236%] |
| `complete_native_candidate_including_cook_io` | +0.4730% [-0.1001%, +1.4975%] | -0.3595% [-9.8149%, +21.8333%] |

`layout_full_shape` is a real stable cold increase, not a borrowed historical waiver. Across AB processes,
median process p50 is 600 ns -> 1700 ns; p95 is 800 ns -> 1900 ns. These medians differ from the paired
percentage estimator above by design. Per operation first-party allocation census is 10 -> 24 allocations,
952 -> 2510 bytes. Each process performs 100 cold operations after five warm-ups.

Source attribution: Layout.cpp compileLayout now validates every explicit association and recomputes
shaderOwnerId for every complete owner. shaderOwnerId constructs a temporary canonical byte string and
sorted pointer vector, whereas A hashes only the name. The fixture still has two owners and the same
fields; it has no new Data ABI payload. This directly identifies added work and allocation sources, but
no per-function instrumented cycle split was collected, so the exact fraction due to hashing versus
allocation/search is UNKNOWN. Structural correctness requires full identity; temporary allocation strategy
is not automatically justified by that requirement.

Suggested bounded next step: measure/replace only canonical identity's temporary construction with an
allocation-avoiding use of the existing hash primitive where its API permits, preserving the same canonical
bytes, full shape checks and collision rejection. Assess assignment lookup separately. No registry, cached
second authority or removal of validation is proposed. This is a proposal, not an implemented optimization
or accepted correctness-cost waiver. A new locked I would require corresponding qualification; this frozen
experiment is finished. Complete candidate p50 remains about 2.29 -> 2.31 ms, but that does not erase the
local cold regression. Other cold intervals remain recorded without claims of statistical equivalence.

## 7. New multi-owner baseline (Validation ON, no old-equivalent percentage)

The following Normal results include the real new typed workload and are not formal old/new gate samples.
Descriptor set counts are actual resident sets (two per slot), not fictitious driver allocation bytes.
C++ heap census is aggregate cold assembly/Invocation construction, distinct from GPU allocation.

- MULTI_OWNER scenes=1 views=4 FIF=2 pipelines=1 shared_code=1 slots=8 oracle=PASS warm_allocations=0; MULTI_OWNER scene_pass_reuses=0
  - MULTI_OWNER_COLD layout_us=29.800 shader_validation_us=1650.100 relocation_native_program_us=1580.200 invocation_us=351.700 heap_count=298 heap_bytes=47136 descriptor_sets=16 descriptor_native_bytes=UNAVAILABLE
  - MULTI_OWNER_HOT unit=us p50=132.400 p95=171.100 max=201.900 qualification=NEW_BASELINE_VALIDATION_ON
- MULTI_OWNER scenes=1 views=4 FIF=2 pipelines=1 shared_code=1 slots=8 oracle=PASS warm_allocations=0; MULTI_OWNER scene_pass_reuses=36
  - MULTI_OWNER_COLD layout_us=12.600 shader_validation_us=1191.200 relocation_native_program_us=1050.400 invocation_us=1087.800 heap_count=338 heap_bytes=58528 descriptor_sets=16 descriptor_native_bytes=UNAVAILABLE
  - MULTI_OWNER_HOT unit=us p50=89.800 p95=202.400 max=232.200 qualification=NEW_BASELINE_VALIDATION_ON
- MULTI_OWNER scenes=1 views=4 FIF=3 pipelines=1 shared_code=1 slots=12 oracle=PASS warm_allocations=0; MULTI_OWNER scene_pass_reuses=0
  - MULTI_OWNER_COLD layout_us=12.500 shader_validation_us=1201.200 relocation_native_program_us=1092.100 invocation_us=923.400 heap_count=446 heap_bytes=70663 descriptor_sets=24 descriptor_native_bytes=UNAVAILABLE
  - MULTI_OWNER_HOT unit=us p50=139.900 p95=181.000 max=204.400 qualification=NEW_BASELINE_VALIDATION_ON
- MULTI_OWNER scenes=1 views=4 FIF=3 pipelines=1 shared_code=1 slots=12 oracle=PASS warm_allocations=0; MULTI_OWNER scene_pass_reuses=54
  - MULTI_OWNER_COLD layout_us=9.800 shader_validation_us=1193.700 relocation_native_program_us=1106.000 invocation_us=1108.500 heap_count=506 heap_bytes=87751 descriptor_sets=24 descriptor_native_bytes=UNAVAILABLE
  - MULTI_OWNER_HOT unit=us p50=60.400 p95=149.200 max=240.100 qualification=NEW_BASELINE_VALIDATION_ON
- MULTI_OWNER scenes=2 views=4 FIF=2 pipelines=1 shared_code=1 slots=8 oracle=PASS warm_allocations=0; MULTI_OWNER scene_pass_reuses=0
  - MULTI_OWNER_COLD layout_us=10.500 shader_validation_us=1165.200 relocation_native_program_us=1068.000 invocation_us=935.700 heap_count=298 heap_bytes=47136 descriptor_sets=16 descriptor_native_bytes=UNAVAILABLE
  - MULTI_OWNER_HOT unit=us p50=132.200 p95=187.400 max=202.100 qualification=NEW_BASELINE_VALIDATION_ON
- MULTI_OWNER scenes=2 views=4 FIF=2 pipelines=1 shared_code=1 slots=8 oracle=PASS warm_allocations=0; MULTI_OWNER scene_pass_reuses=24
  - MULTI_OWNER_COLD layout_us=10.200 shader_validation_us=1198.400 relocation_native_program_us=1081.700 invocation_us=1062.000 heap_count=338 heap_bytes=58528 descriptor_sets=16 descriptor_native_bytes=UNAVAILABLE
  - MULTI_OWNER_HOT unit=us p50=126.800 p95=158.600 max=172.800 qualification=NEW_BASELINE_VALIDATION_ON
- MULTI_OWNER scenes=2 views=4 FIF=3 pipelines=1 shared_code=1 slots=12 oracle=PASS warm_allocations=0; MULTI_OWNER scene_pass_reuses=0
  - MULTI_OWNER_COLD layout_us=11.300 shader_validation_us=1174.500 relocation_native_program_us=1140.800 invocation_us=924.300 heap_count=446 heap_bytes=70663 descriptor_sets=24 descriptor_native_bytes=UNAVAILABLE
  - MULTI_OWNER_HOT unit=us p50=108.500 p95=159.500 max=193.400 qualification=NEW_BASELINE_VALIDATION_ON
- MULTI_OWNER scenes=2 views=4 FIF=3 pipelines=1 shared_code=1 slots=12 oracle=PASS warm_allocations=0; MULTI_OWNER scene_pass_reuses=36
  - MULTI_OWNER_COLD layout_us=10.200 shader_validation_us=1189.600 relocation_native_program_us=1115.900 invocation_us=1243.800 heap_count=506 heap_bytes=87751 descriptor_sets=24 descriptor_native_bytes=UNAVAILABLE
  - MULTI_OWNER_HOT unit=us p50=130.100 p95=235.700 max=615.300 qualification=NEW_BASELINE_VALIDATION_ON

All raw warmed samples and ASan counterparts are retained. Native descriptor-driver byte allocation cost
is UNAVAILABLE; no invented Vulkan allocation-byte estimate. These microsecond values include validation,
so they are not compared with the validation-free formal benchmarks.

## 8. Protection, evidence integrity and disclosed exceptions

Legacy 719/719 blob/mode/size match frozen `a669409a289a6fa4092f21176397795b1cdb7f3e`.
Core, Transport, F2 public/source algorithm, Runtime, Foundation/Queue/Retirement/Record/Sync2 and frozen
FINAL/RD2/historical reports match protected Git objects. Both original workspaces retain HEAD, branch,
status, staged/unstaged/untracked state and seven user-file byte hashes. No main/product modification.
Remote was still approved c640... before delivery; only ordinary fast-forward push is permitted.

Disclosed preflight exception, explicitly accepted for continuation: historical compiler-intermediate
`D:/LuxQualification/render-v2-f6a/qualification/bf3223038d415ed6912dcea04dc8f4d195becd3e/normal/render_vulkan/test/CMakeFiles/render_vulkan_native_graph.dir/vc140.pdb`
changed from SHA256 `0fb7219eedd44b7ebcd8f1a957989abc4df48f54d0eca2d9bec51b905fab0dfd`
to `a22c34adb5bfbfc108400fb8ca9c1f819f8a969777819b676156937715176146`.
Other 3785 archive files match, including final EXE/PDB/LIB/SPV, reports and performance samples.
Original archive manifest remains SHA256 `99377492edd87638546ea8f389221a25ea0a91185a4516221c8ccd521fd0a8dc`.
No claim is made that every historical intermediate remained unchanged; A final binary identity is intact.

One first development initializer-error full log was overwritten before unique log names were enforced;
its compiler error remains in conversation tool output, not a recoverable complete raw log. Later failure
logs remain separate. Initial incorrect native preflight harness and real subsequent rejection are distinct.
External evidence collectors required a 67-to-82 Shader inventory correction and readback extraction handling
for interleaved overlay warnings; these did not change source, artifacts, test outcomes or raw logs.

## 9. Evidence location and manifest

Root: `D:/LuxQualification/render-v2-f6b`.
Final qualification: `D:/LuxQualification/render-v2-f6b/qualification/7406965112f09807b9f70aa3e739ff43539969c8`.
Manifest: `evidence-manifest.json`, **4515 files / 4,436,088,058 bytes**.
SHA256: `97ffe0ed7b5138c25be41d8465a6dfd37ea18aaa76e03bb2fcdec01af25c6d76`.
Coverage/exclusions are explicit in manifest: final binaries/PDB/LIB, generated artifacts, actual build DAG,
commands, raw logs, all formal process samples, preflight and protection evidence. Git metadata, source clone
(identified by tracked I), compiler obj/pch/ilk/intermediate vc140.pdb and manifest/delivery self-reference
are excluded. Prior candidate/development build trees remain retained, not comprehensively rehashed here.

Key receipts: isolation.json, final-audit.json, artifact-identities.json, normal/asan-closure/closure.json,
normal/asan-LastTest.log, independent-readback-oracle.json, final-source-protection.json,
performance-identity.json, performance/{schedule,environment,comparison}.json, performance/rows.jsonl,
performance-audit.json. Root `qual-7406965112f0-*` records exact commands/return codes.

## 10. Final disposition and next-stage boundary

```
F6B_FUNCTIONAL_ABI_GPU_LIFETIME = PASS
F6B_NORMAL = 200/200 PASS
F6B_FULL_ASAN = 200/200 PASS
F6B_FIXED_BINDING = PASS
F6B_RECORD_SUBMIT_P95 = PARTIAL — new interval/control uncertainty
F6B_COLD_LAYOUT = REGRESSION_OBSERVED — not waived
F6B_OVERALL = PARTIAL
F4_COLD_DEBT = PARTIAL — historical limited deferral retained
F5C_RECORD_SUBMIT_P95 = PARTIAL — historical limited deferral retained
F6C = NOT_AUTHORIZED / NOT_IMPLEMENTED
F6D = NOT_IMPLEMENTED
F7 = NOT_AUTHORIZED / NOT_IMPLEMENTED
V2_PRODUCT = EXPECTED_UNAVAILABLE
STOP
```

No general VTune rerun, Android/Linux qualification, full product build or F6.d plugin installed SDK run:
NOT_RUN, outside this work order. New native multi-owner GPU and original F3/F4/F5 Windows qualifications
were actually executed as listed above. Performance costs/uncertainty require user review before freezing
this substage or proceeding; no automatic F6.c entry and no merge to main.
