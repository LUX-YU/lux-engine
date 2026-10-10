# F3 performance completion: test impact contract

This is a tools/documentation change. Production implementation I remains
`0fbbbd6db3aa4ad53283522e4a54d96556b5b478`; existing qualification is
`41b4f25d973239098aac801f147daf09ab3ac640`. Its PARTIAL performance result is not edited.

Changed inputs are the standalone `diagnose_f3.py`, `test_impact_f3.py`, and new F3
documents. There are no CMake, production, public ABI, fixture or shader edits.
The final machine-readable report binds each changed path to old/new Git objects.

`test_impact_f3.py` requires a clean commit and checks:

1. Full production/frozen tree identities and the prior archive manifest.
2. Actual Ninja transitive inputs, compiler dependency records and commands for `all`.
3. CMake File API source/include/link records, compile commands, generated-input
   closure and actual CTest commands, for normal and ASan configurations.
4. Changed paths against those dependencies (including conservative basename matches).
5. All 266 current native/object/SPIR-V artifacts per configuration against the
   investigation lock, plus original benchmark hashes against prior qualification.

Unknown changed categories or actual dependency hits require integration review;
filename classification alone cannot authorize reuse. This narrow tool does not
claim to map arbitrary future production changes automatically.

| Obligation | This change | Evidence/reason |
|---|---|---|
| 137 CTest normal | Reuse prior PASS | Same I and executable inputs; `test.json`, `test.log`, `closure/tests.json` |
| Full applicable ASan 137 | Reuse prior PASS | Same I, dependency ABI and artifacts; `asan-test.json`, `asan-test.log` |
| GPU validation / F3 readbacks | Reuse prior PASS | No native/shader changes; original test logs, SPIR-V/reflection and readback manifests |
| Graph version/Hazard/Scope/oracles | Reuse prior PASS | Graph sources, ABI, formal fixtures and linked inputs unchanged |
| Shader regeneration | NOT_RUN anew | No generator/schema/template/input change |
| Core/Transport/Legacy tests | NOT_RUN anew | Protected trees unchanged; identity verification is sufficient here |
| Performance | New targeted qualification | AA/BB/AB distribution, intervals, controls; original negative samples preserved |
| Script validation | New | Syntax checks and actual independent invocation |
| Protection / evidence integrity | New | Git trees, six user files/diffs, binary hashes, archive hashes |
| Full integration rebuild | Not required if reuse gate passes | No affected target; no production code is rebuilt |

All reused results remain historical results from I, not newly executed tests.
The original archive did not hash every test executable; it did hash the benchmark
executables, generated artifacts and source-bound receipts. The new 266/configuration
artifact lock proves stability throughout this investigation, not a retroactive
binary-hash receipt for every historical test.

For future edits, use actual compiler/Ninja/header/codegen/link dependencies to map
changed inputs to affected targets and their CTest/negative fixtures. Run Graph
oracles for Compiler/public ABI changes; regeneration/reflection for Shader changes;
GPU/ownership for native changes; appropriate ASan for memory/lifetime paths. Unknown
dependencies remain required, never silently reused. Freeze a new I and run affected
stage integration once when production changes, rather than for every intermediate
candidate. No test obligation is removed: the complete 137-test inventory is retained.

Independent evidence/report paths and final reuse decision are recorded in the new
F3 completion verification. FINAL and all historical reports remain immutable.
