# 参考文献与边界：UE-like 算法机制参考，不作为 LuxEngine 资格证据

- Epic Games (2026), *Nanite Virtualized Geometry* / *Nanite Technical Details*, Unreal Engine Documentation: https://dev.epicgames.com/documentation/unreal-engine/nanite-in-unreal-engine ; https://dev.epicgames.com/documentation/unreal-engine/nanite-technical-details 。可参考 virtual mesh streaming、cluster/detail、fallback mesh 等机制；LuxEngine H1 未实现。
- Epic Games (2026), *Lumen Technical Details*, Unreal Engine Documentation: https://dev.epicgames.com/documentation/unreal-engine/lumen-technical-details-in-unreal-engine 。可参考 screen/software/hardware trace、多级 GI/Reflection、distance fields 与历史；LuxEngine H2 未实现。
- Epic Games (2026), *Temporal Super Resolution*, Unreal Engine Documentation: https://dev.epicgames.com/documentation/unreal-engine/temporal-super-resolution-in-unreal-engine 。可参考 history、motion reprojection、shading rejection、temporal accumulation、disocclusion、flicker 处理；LuxEngine H3 未实现。
- Epic Games (2026), *Virtual Shadow Maps*, Unreal Engine Documentation: https://dev.epicgames.com/documentation/unreal-engine/virtual-shadow-maps-in-unreal-engine 。可参考 virtual page/atlas/cache/invalidation/feedback；LuxEngine H4 未实现。
- Epic Games (2026), *Hardware and Software Specifications*, Unreal Engine Documentation: https://dev.epicgames.com/documentation/en-us/unreal-engine/hardware-and-software-specifications-for-unreal-engine 。可参考各高级特性的设备要求，但实际支持以当前用户的 `VulkanDevice::caps()` 和 enabled features 为准。

**参考与验证说明**：这些网页是当前在线机制介绍（核查于 2026-10-13），不是代码、不是某版 UE 的准确内部实现源码，也不是 UE 与 LuxEngine 性能/质量对比结论；各 H1–H6 必须独立开发固定参考数据集、Shader 与 GPU 验收，不能用文献代替产品证据。
