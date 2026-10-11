# F4 test impact and evidence reuse

The stage modifies native Foundation owners, the F3 descriptor owner, their public headers,
and adds one Native Graph execution target. It does not change Core/Transport/LogicalGraph,
Description/Shader generation, FINAL or historical reports. No file-name-only skip rule is used.

Development: build changed target closures with `-j 4 -- -k 0`, run native-graph and candidate
consumers after graph/owner edits, run Foundation tests for queue/memory changes, and compile
real generated shader fixtures when their sources change. Development failures and logs remain
in the external F4/development archive, including the same-family semaphore/barrier defect
and corrected runs. A development test launched after an unsuccessful candidate build is not
qualification evidence; independent qualification requires a successful frozen build first.

Final I: because the existing Foundation ABI/owners affect old F3 GPU consumers, perform one
complete ordinary and one complete ASan bootstrap integration (each two all builds, second
no work). All original 137 CTest obligations remain; six native closure negatives and three
native graph/candidate/benchmark tests are added. Full ASan uses the proven compatible
SPIRV-Cross installation; no annotations are disabled. GPU execution never overlaps builds.

`verify_f4.py` records changed Git objects, actual CMake File API source/dependency/link inputs,
compile commands, Ninja compiler includes, production meta jobs, shader artifacts/hashes,
independent public-header TUs, and the hot consumer link map. Foundation's original GRAPH
rejection remains: the new Graph layer alone depends on neutral render_graph. The hot consumer
links `render_vulkan_graph`, includes Record.cpp, and must exclude Program.cpp, reflection,
relocation, ShaderAssets.cpp and SPIRV-Cross. There is no legacy/root product bridge.

Reuse: V1 historical matrix, unmodified historical R0-F3 verdicts and external raw archives
remain bound to their original SHAs. No such evidence counts as new F4 GPU/ASan success.
Core/Transport/Graph algorithms are unchanged and their final integration regressions run;
there is no reason to repeat their standalone historical performance campaigns. Compare actual
unchanged R4 native benchmark and F3 binding workload against locked F3 binaries. New native
graph workload establishes a fresh baseline; cold compile phase records exclude prebuilt PSO
creation and report total allocation census separately from phase timings.

Independent V must include precise archive/hash references and run/reuse/NOT_RUN distinctions.
Linux/Android/product/installed SDK/Runtime/presentation remain NOT_RUN; install-header sync
is three-prefix byte synchronization, not an installed consumer qualification.
