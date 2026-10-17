# N5.3 lglsl asset integration — implementation checkpoint

Parent f6e393393 (resolve full parent from Git). N5 remains IN_PROGRESS, no independent N5 gate claimed.
Existing engine/toolchain/shader/src/lglsl/LglslEmitter.cpp is compiled only into a real build-time fixture
executable, not a Render library dependency. emitPassGlsl uses one PassShaderContract for both stages,
provisional positions are stable across omitted stage resources. Shared include contains logic using the
injected resource. Actual glslang compiles generated sources; Native's sole LayoutPlan relocates those
SPIR-V descriptors and verifies the reflected layout before creating the Pipeline.

Real GPU Parameters executable consumes generated binaries and retains its64B multi-provider, two-Context,
negative ABI, last-good and20-iteration hot new=0 assertions. Windows Release full all -j4 -- -k0, second
no-work, targeted parameters/lglsl_parameters2/2,1.00s. Validation/SyncValidation errors0; existing EOS warning.
First build exposed missing generated visibility include on the offline tool; corrected to this build's
generated header path, never an old SDK or fake header. Failed attempt retained. Emitter source remains
unchanged; no new Parser or MaterialGraph. Source/tool/output SHA256 manifest emitted by the build.

This validates emission + Native reflection/GPU consumption, not full Material Cook. Existing PassValidation
scalar metadata limitations are not silently declared solved; real Cooked Unlit integration remains N6.
Raw evidence E:/RenderV2Rebuild/V12/evidence/n53-assets. Public API unchanged; exact preceding SDK retained.
Legacy719,user files/history protected. Full N5 independent qualification and N6/N7 remain outstanding.
