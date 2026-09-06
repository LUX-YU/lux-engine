# S6 v3 resumption state (2026-09-06, work in progress)

Not complete, not a freeze declaration. Continue the approved implementation; no merge or push.

## Completed this continuation

- Verified/archived and deleted 167 obsolete Engine build roots. Deleted logical bytes: 145.2078 GiB;
  archives: 0.3236 GiB; calculated net: 144.8842 GiB. No errors. Main/current/baseline roots retained.
  Archive: `E:/SyncForder/CodeRepos/build-archives/lux-engine/s6-v3-cleanup`.
  Tracked inventory/results: `.internal/evidence/s6-v3-cleanup`.
- Toolset worktree `lux-cmake-toolset-s6-relocation`, branch `codex/s6-relocatable-exports`, HEAD `99c3d04`.
  Ported historical c565251 onto current961c63e, preserving the transitive-command reset fix.
  Fixed export-name asset/import paths. Relocation shared/static/interface/dependency/script fixture PASS,
  including prefix containing spaces, original prefix renamed away, and second builds no-op.
- lux-cxx branch `codex/script-codegen-closure`, HEAD `3100f54d0743c5ed94a4ccf5943df04e933de255`.
  Generator tests now registered using the target, not an existing executable.
  Clean clone `lux-cxx-v3r`, build `build/RelWithDebInfo/q2/c`, new toolset, reflection ON: 52/52, 12.26s,
  install and no-op PASS. Earlier core-only34/34 is not the full qualification.
- Engine commits: 3b8f99cb cleanup; 9b175649 sequence benchmarks; 1adea3e2 explicit toolset prefix;
  d4b7f363 generated C++ Event layouts; 7e389f16 installed invalidation/schema consumer;
  e301569c tiny Ability measurement; d8d14c4a declared Ability version hashing fix.
- At d4b7f363: Developer237/237 and Toolchain216/216, both all/install/no-op PASS using new toolset/cxx.
  C++/Lua/FlowForge sequence groups now use NextStep -> Event -> simulation Delay; sequence validates exactly
  3 suspensions/resumes per completed invocation and bounded drain. C++ Event benchmark uses real generated
  contracts for 1/4/16/64 requirements, broadcast or one-target multi-flight.
- Full SDK copy at `D:/LuxV3SDK/{c,d,t,toolset}` plus full copied x64-windows dependencies (~60.5GiB).
  During relocation consumers original install/q2 prefixes were renamed away, then restored in finally.
  `D:/LuxV3Consumers`: 11/12 PASS; Physics consumer assumed Toolchain schema next to Developer package.
  Fixed source consumer to reproject installed canonical Physics declaration through normal Ability codegen;
  corrected consumer is not yet rerun successfully.
- `RunIncrementalClosure.cmake`: source/body/include/ledger/macro/target-option/template/validation/no-op
  PASS in `D:/LuxV3Incremental2`. First attempt only failed due test copying wrong installed template folder.
- `D:/LuxV3Rename`: original/rename/default removal/signature rejection PASS. Version mutation exposed real
  bug: both Ability templates omitted schema_version in scriptAbilitySchemaHash, using default1. Fixed d8d14c4a
  and added VersionedTestAbility static assertions; latest SDK must be rebuilt/copied and test repeated.

## In flight

- Exec session 76664: final-profile runner on clean `lux-engine-v3r` at d8d14c4a, q2 build/install roots.
  Parameters: CxxPrefix install/q2/c, ToolsetPrefix install/q2/toolset, FoundationPrefix install/RelWithDebInfo.
  Runs Developer, Toolchain, Physics OFF, Lua OFF, Lua54 serially. Read q2/qualification.json and logs.
  Do not build/run other tests or benchmarks concurrently with it.
- Baseline worktree `lux-engine-s6-v3-bcompare` branch `codex/s6-v3-reference-driver`, HEAD `04f1b684` clean.
  Runtime remains44b11a60. Benchmark-only ports: c2594b21 C++ Update; b058c395 sequences; 04f1b684 C++ Event.
  Old API adapts CppScriptEventSource::create(description), omits unavailable new Event stats.
  It still needs build/testing with new toolset/cxx. Do not claim measurements from this unbuilt driver.

### Latest continuation (supersedes the in-flight details above)

- Session76664 completed: d8d14c4a Developer237/237, Toolchain216/216, Physics OFF and Lua OFF passed;
  Lua54 initially failed configure because its separate dependency prefix was not supplied.
- Active session25832 now builds/tests Lua54 with explicit dependency
  `E:/SyncForder/CodeRepos/build/deps/lua54-vcpkg/x64-windows`; do not overlap builds/tests with it.
  Runner now accepts Lua54Prefix and preserves other profile records in qualification.json.
- Engine HEAD13d9b417; source runtime+codegen fix d8d14c4a, later5661049d adds real population benchmark groups,
  411a4ffb adds measurement cases. Population groups still need building/testing on final and baseline.
- Bcompare driver now f53fd36b (runtime44b11a60). Ports include sequence, real generated C++ Event requirements,
  population groups and the build-only absent Lua include install fix. No runtime .src or public API was patched.
  The driver has not been built. Old new-Event counters are unavailable and deliberately omitted, not measured zero.
- D:/LuxV3Consumers had 11/12 PASS with original prefixes physically renamed away and restored afterward.
  Physics failed only because schema JSON was assumed next to the runtime prefix. Its source now uses normal
  Ability codegen on the installed canonical declaration; fresh retry remains pending.
- D:/LuxV3Incremental2 PASS: body/include/ledger/macro/target-option/template/validation/no-op. Published outputs
  preserved on validation failure; body-only change does not touch generated files.
- D:/LuxV3Rename failed the newly added Ability schema-version invalidation check and exposed the template bug.
  d8d14c4a fixes both templates and adds VersionedTestAbility assertions; q2/d and /t rebuilt and installed PASS.
  SDK fragments d/t refreshed by copying full installed prefixes to D:/LuxV3SDK; retry must use a fresh output root.
- Tiny IPO consumer now supports --output for four measured nonempty paths (direct/dynamic/static/native entry).
  Correctness consumer old version passed; new timing option not rebuilt/measured yet.
- Script population groups create/invoke/retire two complete physical populations through real ScriptSystem/backends.
  C++/Native reuse their backend owner; Lua factory also includes VM construction, explicitly documented in code.
  These supplement the separate same-runtime Lua churn and one-time lifecycle/retirement probes.

Next serial actions after session25832: finish/rebuild population tests at latest clean commit; run remaining
relocated Physics and schema reimport; build Bcompare driver with new toolset/cxx; run formal paired measurements,
stress, VTune/assembly and final evidence. Formal performance has not yet been run. No completion claim.

## Next work

1. Finish/read q2 matrix; fix genuine failures and bind results to exact commits.
2. Recopy newly installed SDK fragments to D:/LuxV3SDK (not manual installed-file patches).
3. Rerun Rename/schema test with fresh root, Physics/IPO consumer and invalidation as needed.
4. Build baseline driver using matching deps; add baseline adaptation only in benchmark code if needed.
5. Finish measurement runner scenarios/strict validity. Formal five-pair runs NOT RUN yet.
   B06 full backend lifecycle sizes, B09 worker comparisons, B10 graph measurements still need data.
6. LuaJIT interpreter suite, Lua54 focused suite, selected100x stress, dependency/header-sync/validators,
   final relocated consumer checks and actual disassembly/VTune.
7. Final report only candidate if all required implementation and data complete; otherwise exact BLOCKED.

## Important retained paths/limitations

- Do not delete or overwrite Bcompare `deep-developer` bin: Native fixture paths are embedded.
- `v3bt` existing reference Toolchain must be rebuilt after driver ports; it currently contains older driver.
- `s6-v3-evidence/Bentry` and `/Bcompare` retain original bins, DLL/PDBs/configs/manifests.
- Main a577c494 still has five user modifications, untouched. Other projects may build concurrently; never kill them.
- RelWithDebInfo only. No Debug/Release/Player/Editor/Android/Linux builds. Lua54 is requested for affected closure.
- No subagents authorized. Use apply_patch. All builds target all -j4 -k0; serial with tests/performance.
