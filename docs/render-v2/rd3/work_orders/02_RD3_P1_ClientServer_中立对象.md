# RD3-P1 — Client/Server 与中立 Scene/View/Target/Effect IDs（DRAFT，NOT_AUTHORIZED）

## 前置

D0 已获用户批准和独立 V，当前 `codex/render-v2` HEAD/真实 CMake include closure 已重查。报告 F6.b performance debt 和 F4/F5 deferred debt 仍独立保留，不由新设计扩张为重写 Vulkan Core。提交具体 ALLOWED 文件名单才进入施工。

## 核心任务

1. Public `RenderRuntime` 从现有 `RenderRuntimeBuilder::backend()` Native 装配依赖中解耦，保留 R2 Transport 的 PROGRAM/CONTROL/UPLOAD、Reply、Stop/Drain、错误及 thread rules；应用/Server 初始化负担选择具体 Native factory，Client 不认识 Vulkan headers。
2. 在中立层建立 `Scene` 与 `View` 的唯一物理容器：Backend owns Scene SlotMap；每 Scene owns View SlotMap/Handle；不允许历史 `RuntimeSceneState` + 新 Scene 两份同时作为长期可变 authoritative state。
3. 中立 `Target`/`TargetDesc`/`TargetLayer` 保存最终输出语义、Format/Extent/Resize/History/Layer 合同，Native Surface/Swapchain 独立移向 Vulkan 后端；View 的唯一 Effect 选择字段初始化和正确生命周期，虽 P2 才完整实现 Effect。
4. Backend owns ResourceDomain table 的必要 `ResourceDomainId`/parent 预埋边界，但不在此阶段先写完 GPU asset cache 或第二 ECS。
5. Error/capacity/device identity/wrong generation/multiple Scene shared client、Device Lost/Stop/Ctrl rollback 及无 PROGRAM 独立帧节奏测试。旧 Vulkan GPU Frame fixtures 继续通过。

## 必须拒绝

为了“隔离”再引入 `RenderApplication`、`IBackendService`、`SceneManager`、`ViewManager`、第二个 worker 或 Socket 工厂；Client 直接持 VkDevice 或把 Backend-native handles 塞进 Scene 公共头；仅重命名而未删除复制的 RuntimeSceneState/ViewState。

## 证据与 Gate

MSVC C++20 standalone public headers/安装后消费、Window/Offscreen/2 Scenes/4 Views GPU fixtures、ASan、Request stop/create thread failure、未 READY 状态/故障注入、F4/F5 原 performance baseline impact/零热分配、clean-clone I/V，719 Frozen + user workspace 原字节保护。P1 PASS/未过不得进入 P2。独立 V 只新验收文档，Push 后 STOP。
