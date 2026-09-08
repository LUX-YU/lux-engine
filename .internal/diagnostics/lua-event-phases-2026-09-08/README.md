# Lua Event phase probe

This is a diagnostic TU, not a production target. It includes the qualified benchmark fixture,
reuses its generated objects, and links the actual production DLLs. No additional CMake tree is created.

The local scripts pin the existing Windows Developer build/source/artifact paths. To replay elsewhere,
provide equivalent qualified inputs and update those paths explicitly; do not silently use another SDK.
The artifact directory argument must be new for a fresh run. The raw archive includes the original
commands, failures, identities and per-process CSV files.

1. Verify the qualified source and installed dependencies. Use only RelWithDebInfo.
2. Run the existing build `cmake --build <o/w/d> --target all -j 4 -- -k 0` serially; record actual images.
3. In the MSVC amd64 developer environment: `python -B build.py <artifact-directory>`.
4. Serial stages: `python -B run.py <artifact-directory> smoke`, `hardware`, `profile`, `reports`,
   `phase-reports`, `memory`, `timing`, `assembly`. Do not run these alongside builds/tests/other benchmarks.
5. `python -B analyze.py <artifact-directory>` generates derived `analysis.json`.
6. `audit.py` compares the recorded identity-start manifest to the final files and preserves local images.
   It intentionally requires the original manifest; it does not invent a fresh baseline.

`LUX_DIAG_ITT` is set only for profiling. Counters/validation/CSV output are outside timed operations.
The four phases are register, deliver, clock and resume_cleanup. VM accounting is enabled only in the
separate memory runs. Zero allocation columns in timing runs mean unobserved, not allocation-free.

ITT task filtering is authoritative: tail-call optimization removes some noinline wrappers. VTune
inclusive percentages overlap; physical and inline presentations must not be added. The analyzer
handles the indentation VTune puts before quoted CSV fields.

The five original/probe pairs control observation perturbation. They are not two production candidates.
There is no per-instance Lua field readback or per-waiter latency timestamp in this probe.

See [the analysis report](../../script-lua-event-phase-investigation-2026-09-08.zh-CN.md)
and [raw evidence](../../evidence/script/lua-event-phases-2026-09-08/README.md).
