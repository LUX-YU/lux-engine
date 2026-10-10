# Evidence / source provenance (not a test report)

- Legacy base: `a669409a289a6fa4092f21176397795b1cdb7f3e`. Original R2 design bundle: user upload `LuxEngine_RenderV2_FinalDocs_R2(2).zip`.
- Current reviewed branch at document compilation: `7de3ddaa3f7614995745b41d73dfc98302310a4c` (`codex/render-v2`, R4 verification). The implementation agent must verify actual remote HEAD at F0.
- R2-FIX I/V `4ae55059cca949178a8a1f2eaffe32f720384e8d` / `e0eabe1640d94fafb80a57bd334507162f3b2f9f`; R3 I/V `ed4b78c1a4606f917eec1b30c654567935e6de48` / `f9b943c9e768ef2c9d408962f0d5a32d2cf3de74`; R4 I/V `83ffbb6d9859d87ca62a8a2acbb0083c2a8de420` / `7de3ddaa3f7614995745b41d73dfc98302310a4c`.
- R0/R1 earlier Git identities and precise tests are described in historical reports; **do not alter their conclusion**.
- This final package is a target design and test requirement. It is **not** an additional clean clone verification or a claim that any F-phase, legacy parity feature or UE algorithms have been newly built.
- Direct frozen evidence source path for all matrix F/I rows: `render_legacy/**`, `engine/scene/builtin_systems/render/**`, `engine/toolchain/shader/**`, `modules/resource/description/**`, as mapped per row. Existing Design Draft was consulted but is not authoritative; Final 00–19 decide conflicts.

## Official technical documentation (reference, not copied API)

- Unreal RDG: https://dev.epicgames.com/documentation/en-us/unreal-engine/render-dependency-graph-in-unreal-engine
- Nanite: https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-virtualized-geometry-in-unreal-engine
- Lumen: https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-global-illumination-and-reflections-in-unreal-engine
- TSR: https://dev.epicgames.com/documentation/en-us/unreal-engine/temporal-super-resolution-in-unreal-engine
- VSM: https://dev.epicgames.com/documentation/en-us/unreal-engine/virtual-shadow-maps-in-unreal-engine
- Substrate: https://dev.epicgames.com/documentation/en-us/unreal-engine/substrate-materials-in-unreal-engine
- Vulkan Registry: https://registry.khronos.org/vulkan/specs/latest/html/
- SPIR-V Registry: https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html

This document maps technical concepts to public references; Vulkan device capability and shader compiler contracts must be proven against the actual deployed SDK/driver.
