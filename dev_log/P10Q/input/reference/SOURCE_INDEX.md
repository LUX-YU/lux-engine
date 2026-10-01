# 固定源码索引

实现：`11de2c1fee9d477daaa2d26b07911306ea9d6b9f`；验收：`b9b856477755a9247a7a6810fa880ae62151f565`。

下列 Git Blob SHA 来自连接器返回的元数据，不表示本审阅环境重新下载并计算了所有源文件的哈希。源代码引用固定到提交；归档只作为实施方实测记录。

## S01 · 提交链
[Git refs / commit metadata](https://github.com/LUX-YU/lux-engine/commit/b9b856477755a9247a7a6810fa880ae62151f565)

- 读取内容：GitHub connected metadata; head parent is the implementation
- 类型：提交元数据

## S02 · Host 与拥有权
[editor/desktop/src/ViewHost.cpp](https://github.com/LUX-YU/lux-engine/blob/11de2c1fee9d477daaa2d26b07911306ea9d6b9f/editor/desktop/src/ViewHost.cpp)

- 读取内容：adopt, drain, detach, bounded slots
- 连接器 Git Blob：`ee8715aae06391a031fab9187cff23bea5363eca`

## S03 · Flow UI 全部主要路径
[editor/tools/flowforge/ui/src/FlowView.cpp](https://github.com/LUX-YU/lux-engine/blob/11de2c1fee9d477daaa2d26b07911306ea9d6b9f/editor/tools/flowforge/ui/src/FlowView.cpp)

- 读取内容：properties_/properties_base_, select, Properties::draw, install, maintain, public delegates; retrieved ranges cover complete source
- 连接器 Git Blob：`8a390136eaf608f49cb625cdcba906f9e0923e3a`

## S04 · Material UI 全部主要路径
[editor/tools/material/ui/src/MaterialView.cpp](https://github.com/LUX-YU/lux-engine/blob/11de2c1fee9d477daaa2d26b07911306ea9d6b9f/editor/tools/material/ui/src/MaterialView.cpp)

- 读取内容：CanvasRequest, callback, maintain, APPLY_NODE existing protection; read relevant ranges, not every cosmetic line
- 连接器 Git Blob：`1fbc37fec1c0fc3a8f86544edefad4b00338c0fc`

## S05 · 通用画布
[editor/widgets/src/GraphCanvas.cpp](https://github.com/LUX-YU/lux-engine/blob/11de2c1fee9d477daaa2d26b07911306ea9d6b9f/editor/widgets/src/GraphCanvas.cpp)

- 读取内容：displayed node positions; edited delivery; no Session dependence
- 连接器 Git Blob：`122d759d003aa8807f03fe4a266a672ad1ce7f94`

## S06 · 原交互边界
[editor/tools/flowforge/interaction/src/FlowInteraction.cpp](https://github.com/LUX-YU/lux-engine/blob/11de2c1fee9d477daaa2d26b07911306ea9d6b9f/editor/tools/flowforge/interaction/src/FlowInteraction.cpp)

- 读取内容：begin takes current, preview/commit reject stale; no new implementation needed
- 连接器 Git Blob：`d12856360ca3e8f37b0bffc179e6885b3c227ddf`

## S07 · 原作者模型
[editor/tools/flowforge/model/src/FlowSession.cpp](https://github.com/LUX-YU/lux-engine/blob/11de2c1fee9d477daaa2d26b07911306ea9d6b9f/editor/tools/flowforge/model/src/FlowSession.cpp)

- 读取内容：apply expected check, original history, stable reads
- 连接器 Git Blob：`986cad9e9701006385d3866cc0a789f533300ab3`

## S08 · 正式新视图测试
[editor/tools/scene/ui/test/views.cpp](https://github.com/LUX-YU/lux-engine/blob/11de2c1fee9d477daaa2d26b07911306ea9d6b9f/editor/tools/scene/ui/test/views.cpp)

- 读取内容：read fixture and Flow matrix; apply_property uses begin/preview/commit with fresh test edits
- 连接器 Git Blob：`e89dc80904e060ccd8a566db881e0f04aca95abf`

## S09 · 真实 GPU/输入归档
[dev_log/P10/logs/desktop-detail.log](https://github.com/LUX-YU/lux-engine/blob/b9b856477755a9247a7a6810fa880ae62151f565/dev_log/P10/logs/desktop-detail.log)

- 读取内容：host; dual SceneView readback; native input; IME explicit untested
- 连接器 Git Blob：`2b7bfb050aa6aedef8be66576c99d133aa119754`

## S10 · CTest 归档
[dev_log/P10/logs/ctest.log](https://github.com/LUX-YU/lux-engine/blob/b9b856477755a9247a7a6810fa880ae62151f565/dev_log/P10/logs/ctest.log)

- 读取内容：read ending summary 185/185; not independently executed
- 连接器 Git Blob：`c085ff66394745bd6d93735b357b8461083b8513`

## S11 · 归档检查器
[dev_log/P10/check_receipt.py](https://github.com/LUX-YU/lux-engine/blob/b9b856477755a9247a7a6810fa880ae62151f565/dev_log/P10/check_receipt.py)

- 读取内容：read source; not executed; preserved test mapping and SDK/GPU mode checks
- 连接器 Git Blob：`d2b057d9a6133e51501c0e3a76b1d42980e87c1d`

## S12 · Inspector 对照
[editor/tools/scene/ui/src/InspectorFields.cpp](https://github.com/LUX-YU/lux-engine/blob/11de2c1fee9d477daaa2d26b07911306ea9d6b9f/editor/tools/scene/ui/src/InspectorFields.cpp)

- 读取内容：stable display capture and active draft stale check
- 连接器 Git Blob：`50b68375369d377bcd610eefeb36fb5ac753547b`

## S13 · DesktopShell
[editor/desktop/src/DesktopShell.cpp](https://github.com/LUX-YU/lux-engine/blob/11de2c1fee9d477daaa2d26b07911306ea9d6b9f/editor/desktop/src/DesktopShell.cpp)

- 读取内容：composition and frame/update handoff
- 连接器 Git Blob：`4441b5cc5b430eb41b17128d1e447a4dae11df43`

## S14 · 原生输入源码
[editor/tools/scene/ui/test/NativeDesktop.hpp](https://github.com/LUX-YU/lux-engine/blob/11de2c1fee9d477daaa2d26b07911306ea9d6b9f/editor/tools/scene/ui/test/NativeDesktop.hpp)

- 读取内容：read first 180 lines; SendInput and Inspector/mouse capture paths
- 连接器 Git Blob：`70ef05531ae85abb4cbe13562c002403ad6ba488`

## S15 · 窄视图拥有单元
[editor/views/api/include/lux/engine/editor/views/IViewHost.hpp](https://github.com/LUX-YU/lux-engine/blob/11de2c1fee9d477daaa2d26b07911306ea9d6b9f/editor/views/api/include/lux/engine/editor/views/IViewHost.hpp)

- 读取内容：DetachedView move-only and prepare-close hook
- 连接器 Git Blob：`454350ac61bbe7767e0ebb4a25cff273830dd4cb`

## S16 · Host 回归
[editor/desktop/test/host.cpp](https://github.com/LUX-YU/lux-engine/blob/11de2c1fee9d477daaa2d26b07911306ea9d6b9f/editor/desktop/test/host.cpp)

- 读取内容：real root requests/generation/callback batches
- 连接器 Git Blob：`b15d33d82f4ff7586ee6ecff30e6efd459125eb6`

## 本地参考

- [原 P10 启动补充](reference/P10_START_ORIGINAL.md)：此前约定，不是本轮新证据。
- [用户 P10 README](reference/P10_SUBMITTED_README.md)：用户提供的提交说明，逐字复制。
- [用户 P10 FILES](reference/P10_SUBMITTED_FILES.md)：用户提供的变更清单，逐字复制。

本包没有内置生产源码副本或假装已可编译的替身 SDK；测试计划必须在原工程环境落实。