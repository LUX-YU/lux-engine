# F3 Shader author and binary contract

`//! lux-pass-declarations` is the unique generated-declaration insertion point. It is a line directive
in the existing emitter, not another preprocessor. Version precedes other effective directives;
extensions precede prerequisite includes/types. Prerequisites precede the marker; helpers using
generated resources follow it. glslc resolves all includes, macros and conditional branches.

Without the marker, the existing leading comment/extension/include/define prefix is a prerequisite
prefix, and generated declarations follow it. Helpers that consume generated resources require the
explicit marker; an undeclared use is a compiler error. Neither include filenames nor their contents
are inspected to guess dependencies. Duplicate markers, late version and late extension directives
are emitter errors. Existing per-stage declarations and physical-binding rejection remain active.

`F3Includes.py` executes production emitters, glslc, spirv-val and SPIRV-Cross reconciliation. It saves
commands, statuses, source/generated/binary hashes and diagnostic logs. LegacyTonemap remains an
unchanged source fixture. Existing stage program and ABI/type/count/access negatives remain mandatory.

Color Clear is a closed value constructed from `FloatColorClear`, `SintColorClear` or `UintColorClear`.
Its private discriminator/union preserves standard layout under MSVC; the initial std::variant attempt
failed the existing generated standard-layout assertion and was replaced without weakening that gate.
Format classification remains solely in neutral `rdesc::Image.hpp`. CLEAR validates the category;
LOAD/DISCARD ignore unused clear values. Exact values are captured into invocation storage and do not
enter the logical structural key. GPU attachment-clear qualification is still required in F3.4.

Development F3.0 run: 126/126 tests, including all 124 baseline names, on the mutable construction tree.
This is internal gate evidence, not the final independent qualification or a F3 PASS claim.
Raw log: `E:/SyncForder/CodeRepos/archives/lux-engine/RenderV2/F3/development/f30-full-test.log`.

## Cooked variant, binary relocation and native validation

`ShaderBuildInputs` owns exact source/include bytes, defines, compiler identity and target environment.
The asset/variant stable name uses the existing author ShaderReference contract; `#` is reserved for
its separator. `CompiledShaderVariant` owns these facts, generated-schema serialization and every actual
stage binary. Complete-program validation requires the right V/F or C stage set, shared field agreement
and matching vertex outputs/fragment inputs. The bounded cold catalog compares complete identities;
publish/remove returns the displaced owner, and lookup borrows end at mutation/destruction. There is
no watcher, native GPU ownership, global registry or frame lookup.

Relocation supports SPIR-V 1.0–1.6, native endianness, one Vertex/Fragment/Compute entry point per module
and direct paired DescriptorSet/Binding decorations. It first validates and reflects the original, then
collects both positions for each target before changing either, then writes the final LayoutPlan pair.
Duplicate/missing/grouped decorations, aliasing, malformed instruction lengths, extra resource interfaces
and wrong entry stages fail. The owned result retains original and patched words plus the exact mapping.
SPIRV-Tools validates both binaries under Vulkan 1.3; existing SPIRV-Cross reconciles both against the same
generated schema (provisional versus final locations). There is no second shader parser or schema registry.

`reflectPassInterface()` uses that same cold SPIRV-Cross boundary: numeric 32-bit stage IO, arrays/matrices
expanded to locations, scalar category/width/components and interpolation. Interface blocks, 64-bit IO
and dual-source outputs fail explicitly. Native creation reconciles vertex attributes, fragment target
categories, stage component limits and actual enabled capabilities. It rejects unsupported capability
declarations and LocalSizeId (maintenance4 is not enabled), and validates literal compute workgroup limits.
This avoids accepting physical support as enabled device functionality.

The toolchain links the installed SPIRV-Tools **C ABI**; no external source or ABI is changed. Normal and
full MSVC ASan builds use the appropriate SPIRV-Cross binaries with matching STL annotations. Heavy
SPIR-V headers stay in implementation files. The fixed native binding object links without cold compiler,
catalog or SPIR-V reflection objects. Raw generated GLSL, original/final SPV, schema, maps and hashes are
qualification artifacts. Specialization default values are part of exact SPIR-V identity; runtime
override maps are not exposed in F3. Graphics/compute variants are cooked before native publication.
