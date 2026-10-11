# F5 Native 接缝影响审查

批准来源：RD2-F5 正式授权要求先给出真实消费者、最小失败证明与文件影响，再实施必要的下层修正。这里不扩大到 F2/Core/Transport/Generator。

## 消费者与已复现边界

消费者是 F5 的一个 Scene、同一 render frame 中四个 View，以及三个在途 frame 的唯一 Backend。
四个 Invocation 的完成和读回责任必须独立于 frame ordinal。

锁定 `92b126f67873ccbb576d0ffc5f1330964d6f84f6` 独立检出的既有 production libs，编译并运行
`RD2-F5/preflight-719f3d06/NativeInvocationEntry.cpp`（源码树外证据）。真实 Vulkan copy/readback 使用同源生成的 F4CopyBuffer。

结果：frame slot 0/1 的两次执行取得正确的 11/22/33/44；slot 2/3 均在提交前返回 InvalidArgument；复用 slot 0 后第一张回执返回 WrongOwner；frame_capacity=4 在冷编译时拒绝。Validation 0 error。
这不是 F4 在其原两/三 frame 合同中的 Bug，而是 F5 无法将独立 Invocation 身份表达为既有 frame_slot 的可执行证据。
命令、完整日志、可执行文件和输入库 SHA-256 保存在 `entry-commands.json` 及相邻文件；不修改旧资格二进制。

## 变更前的所有权问题

`pinclude/GraphNative.hpp::NativeGraphBacking` 同时包含 immutable programs/recipes/cells 和 mutable slots。
`Compile.cpp` 持续保存初始 `NativeImportBinding` 的 backing 引用；`Resources.cpp` 为每个 slot 配置实际 backing；`Recipes.cpp` 还在初始 backing 上建立 ImageView/Descriptor。
`Record.cpp` 直接用 GraphFrameValues.frame_slot 选 slot。不能通过 View 数量扩大该 frame 字段、每 View 重编译一份计划，或 waitIdle 串行执行解决。

初始 Import 借用的现状属于需移除的长期装配耦合；本记录没有把未触发的悬空访问宣称为已复现 Bug。

## 有界修改范围与回归义务

| 现有文件 | 所需职责修正 | 必需回归 |
|---|---|---|
| vulkan/include/.../graph/Executable.hpp、pinclude/GraphNative.hpp | 不可变 Plan 与独立有界 Invocation 资源 owner 分离；frame ordinal 与 invocation slot 分离；回执带所属执行 owner 证明 | 同计划四 View；2/3 FIF；错误 owner/过期回执；公开头 |
| vulkan/src/graph/{Compile,Resources,Recipes,Frame,Record,Export,Diagnostics}.cpp | 冷态结构规划与 Invocation backing/descriptor 装配分离；实际 Import 仅按本次执行及 GPU last-use 保活；沿用 Sync2/alias/queue 算法 | 原 F4 C1/C2/C3、所有 Draw、alias、多队列、export/retirement、faults；Full ASan |
| vulkan/src/graph/Copy.cpp（如 extent 解析所需） | Target signature 冷态确定实际 extent，copy/readback 的物理尺寸与之相符；不改 F2 版本和访问算法 | Resize 的独立像素 oracle，旧 mip/copy 范围负例 |
| vulkan/transfer/Submission 公共和实现接缝（如真实 Presentation 所需） | 有界 binary acquire/present semaphore 与既有 timeline completion 接入，继续唯一 Queue owner | 原 Submission/Fault/Retirement；真实 Present/stop/resize、SyncValidation |
| vulkan/test 既有消费者 | 迁移 ownership 装配，不删语义 oracle、不改变正式性能工作量 | 原 156 测试义务逐项对应 |

上述目录均指 `modules/function/render/vulkan/`。F5 新 Runtime、Backend、Scene/View/Target 实现与 bootstrap 测试另外列入本阶段类型/依赖清单。
若过程中证明必须改 F2、Core、Transport、生成器或冻结设计，停止该越界项并提交最小证据；不得在上述审查名义下扩大权限。

## 性能归属

Plan/Invocation 分离会改变 C10/C11 的计时边界。等价冷态比较必须包含原来的同数量资源、描述符、Shader/Pipeline 创建和销毁，不能只测新的纯 Plan 工厂后宣称改进。分段数据另列。
开发期只编译受影响 targets；最终 I 上执行一次完整 Normal/Full ASan/真实 GPU/closure。四 View/多 Scene 的热态单独建立真实工作量基线；F4 冷 p95 债务继续引用入场记录，不开展无界重复测量。

本文是实施前影响审查，不是完成声明；Native Invocation、Presentation 与 Runtime 的新行为尚未取得 F5 资格。
