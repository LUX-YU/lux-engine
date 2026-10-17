# N5 structured Buffer ABI — implementation checkpoint

Parent `425cdfa98de08ecef0536d65a75e780ee969f361`. N5 remains IN_PROGRESS; no push.

ShaderBinding now carries an owned ShaderBlock rather than a single byte count. ShaderBlock copies existing
rdesc::DataGpuLayout names and field facts; ShaderMember and ShaderBlock are cold values, not new Owners or
registries. Full equality includes field order/name/scalar/offset/shape/strides and runtime-record representation.
Shader metadata keeps source-local storage alive after Cook/configuration inputs disappear. DataLayout.hpp
is installed alongside the existing neutral metadata; no Engine or Vulkan dependency is added to Data.

Native uses the audited DataValidation.cpp SPIRV-Cross field walk adapted to its existing C API. Actual shader
member/array/matrix facts are checked at compile; malformed inputs never publish a pipeline. Original SPIR-V
validation and controlled relocation remain. Accepted scope is column-major 32-bit numeric fields, nested
structs, fixed one-dimensional arrays and an explicit single zero-offset runtime record array. Unsupported
row-major/multidimensional/other-width forms reject; broader ABI and general push parameters are unfinished.
No Shader parser or additional graph/descriptor authority was introduced.

Real GPU test uploads a 96B mat4 + vec4[2] buffer through the existing external TestBuffer Storage and Transfer,
then samples Texture + buffer through relocated bindings. Both ordinary blocks and stride96 runtime records
produce RGBA32 at every pixel. Ten wrong member/stride/offset/type/name/size/alignment/representation cases
reject cold. Original float4 buffer fixtures now declare their real full layout and retain their assertions.
The external test Storage accepts configured byte size; its ownership and Transfer algorithm are unchanged.

Windows RelWithDebInfo all -j4 -- -k0, second no-work, full CTest -j1: **109/109,125.57s**.
Two new tests; all previous107 names retained. GPU Validation/SyncValidation errors0; actual EOS overlay
warning retained. Initial targeted4/5 failure was loader0xc0000135 before execution: the new test needed the
same IPC DLL runtime output directory as existing external-Storage tests. Corrected CMake, actual GPU rerun
passed. Failed attempt remains in external evidence; it was not counted as a GPU test success.

Protection checker:719 Legacy blobs,3 user files, dependencies, historical reports and boundary unchanged.
Source adaptation hash is N5-BLOCK-sources.json. Generated shader input/compiler/output hashes and all raw
build/test logs are under E:/RenderV2Rebuild/V12/evidence/n54-block. Exact-I independent ASan is next.
No performance improvement claim: historical performance PARTIAL and Root TOOLCHAIN BLOCKED remain.
General parameter composition, lglsl assets, graphics/compute primitives and N6/N7 remain to implement.
