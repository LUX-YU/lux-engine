# Render V2 架构与使用入口

Render V2 的源码位于 `modules/function/render`，通信使用独立的 Platform IPC。
Render 作为一个共享库提供逻辑图、Feature、资源和执行能力，不依赖 Engine 或 Editor 产品。
设计与审查遵守 [通用软件设计与 C++ 工程规范](../engineering-principles.md)；
当前 API 和运行合同见 [Render README](../../modules/function/render/README.md)。

- [从请求到图像的完整流程](rendering-flow.md)：当前类的职责、调用链、线程和释放顺序，供源码审查。
- [Engine/Editor 接线设计](engine-editor-integration.md)：共享执行资源与 Client 运输，以及尚未实施的完整 Scene、窗口与 UI 接线。

## 目录与职责

顶级目录统一为 `include/`、`sinclude/`、`pinclude/` 和 `src/`，分别表示安装公开头、
项目扩展头、模块私有头和实现。下表是库内职责目录，不是多套独立库 target。

| 职责目录 | 内容 |
|---|---|
| core | 基础身份、结构化错误、独占配置值和执行统计 |
| commands | 中立 Command 声明、请求和结果值 |
| graph | 唯一逻辑 Graph、Builder、Analysis 和冷态 PassProgram |
| features | 冷态资源供需解析和单 Graph 装配 |
| resource | 专业资源合同、StorageSet 所有权与 ResourceRegistry 非拥有索引 |
| storage | 用途明确的专业 Storage 描述、数据合同及私有实现 |
| device / memory / upload / execution | NRI 设备设施、分配、上传、帧资源、录制提交和物理清理 |
| runtime | Server 对象、Command 路由、关联与请求终态 |

当前选择 NRI Vulkan 后端。NRI/Vulkan 类型不进入安装公共头；需要 GPU 的项目扩展显式使用
`sinclude/` 接缝。Platform IPC 位于独立模块，提供进程内类型化消息、SPSC 和来源关联。

稳定名称用于冷态发现；Command、Handler、Scene、View、Effect、Storage 等运行时身份各有独立身份域。
Scene 拥有 View，Effect 拥有不可变 RenderPlan 及同源 Graph/Analysis，Attachment 拥有 RenderGraphInstance。
Target 拥有实际输出图像，同一 Target 只有一个 Attachment。
StorageSet 独占专业 Storage；Registry 只定位实例。Client 显式管理资源间的业务依赖。

DeviceSession 独占设备设施和物理 Queue 集合。上传保留具体 stdexec Sender，物理清理只进入
Session 的 `cleanup_scope_`。资源在 Copy/Acquire 接纳、使用顺序成立并完成发布后返回句柄；
实际 GPU 完成决定物理资源何时可以复用或释放。没有对外 Ticket 或生产 Readback Command。

帧槽在参与的物理 Queue 完成且 CPU 写入结束后复用。默认两个在途帧，可配置一到三个。
动态 Storage、RenderGraphInstance 和 Target 使用对应物理 slice；CPU 已采用数据、Shader 和 Pipeline 保持唯一。
FrameExecution 负责帧准备、批次依赖和提交合并，RenderExecutor 按计划录制，DeviceSession 提供设备设施和完成推进。
上传由 ResourceUploader 执行 stdexec 流程，物理清理只有 DeviceSession 的一个 `cleanup_scope_`。
当前输出仍是 OffscreenTarget；Surface/Present 以及 Engine/Editor 接线不能从离屏验收中推定完成。

## 宿主与 Client

应用创建一个 Core `ExecutionRuntime`，调用
[`RenderServer::create`](../../modules/function/render/include/lux/engine/render/runtime/RenderServer.hpp)
装配 Server，并在冷态注册 Feature/Storage 描述。应用拥有线程；Server 借用同一个 CPU scheduler。
具体内置实现不公开，不需要应用构造 Provider 或包含专业 Storage 实现头。

Client 直接使用 `IpcRequester` 和 `IpcMessage`，自行生成 RequestId，通过普通目录请求取得 CommandId。
Render 库没有 RenderClient 包装。Engine 可使用 `engine/rendering/RenderRequester` 复用有限帧载体、
类型化 Sender、RequestId 关联和回复消费；它不拥有 connected OperationState 或 GPU 对象。两个 Client 的 RequestId 数值可以相同，回复仍沿原 Link 返回。
发送成功转移消息值；发送失败保留原消息。Response 包含显式 `RenderError error` 和业务字段，
业务字段只在成功时消费。回复不自动返还原 Request。
断连只说明结果无法送达，不保证已接纳的业务被撤销。

上传源是借用：Client 从提交成功到收到 Response 保持源地址、范围和内容有效。
FAILED/CANCELLED 同样在最后一次 CPU 读取结束后产生；abandon 或断连不证明读取已经结束。
没有 Response 时，应用清偿 Server 和 Producer 后才能释放源。Render 独立管理 Staging 和已提交资源。

IPC 使用同进程、同受控 C++ 构建合同。`get<T>()` 的调用者必须知道消息内的实际类型。
宿主必须保持提供类型操作与回调的模块已加载，直到所有相关值析构及回调栈返回。

Server 同步请求直接结算；异步 Command 的 OperationState 由 `counting_scope + spawn` 管理。
既有 RunLoop 将完成交回 RenderThread，ResponseOutbox 统一发布并处理背压，不重复执行 Handler。
停止时先关闭准入，清偿 CPU/GPU、RenderThread 事件、Producer 尾栈和回复；逻辑资源移交清理，
相关完成条件结算后再释放设备设施。

## 构建与可运行消费者

独立构建入口是 [`cmake/render-v2-bootstrap`](../../cmake/render-v2-bootstrap/CMakeLists.txt)。
需要安装同级 lux-cmake-toolset、lux-cxx 及实际后端/异步依赖；NRI 使用锁定版本和共享依赖缓存。

```sh
cmake -S cmake/render-v2-bootstrap -B ../build/render -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON \
  -DCMAKE_PREFIX_PATH="<toolset-prefix>;<cxx-prefix>;<dependency-prefix>"
cmake --build ../build/render --target all -j 4 -- -k 0
ctest --test-dir ../build/render --output-on-failure -j 1
cmake --install ../build/render --prefix ../install/render
```

安装消费者通过 `find_package(lux-engine-render CONFIG REQUIRED COMPONENTS render)`，
链接唯一 `lux::engine::function::render`。中立 Command/资源声明不需要 GPU 类型；
Server 的异步接口按实际需要使用 stdexec。
完整 Engine/Editor 产品接线是独立构建范围，不能用 Render bootstrap 成功替代。

[`render/test`](../../modules/function/render/test/CMakeLists.txt) 保留同步 Client、Native 输出和
Main/Asset 双 Client 的 Mesh/Material 场景。Client 翻译单元不包含后端头；测试自行进行
普通 Copy 和映射来检查输出，不在生产 API 中添加 Readback。

[色彩空间约定](color-space.md) 说明采样与工作色转换；
[Legacy 参考边界](LEGACY_MANIFEST.md) 说明冻结源码的用途与校验方式。
阶段报告和历史资格从 Git 历史查询；原始验证输出保存在源码树外。

### 创建与声明式安装

```cpp
auto server = RenderServer::create(config, execution, device_options);
if (!server)
{
    return server.error();
}
auto mesh_feature = (*server)->featureId<MaterialMeshRasterDescriptor>();
```

`create()` 从私有 `BuiltinFeatures` 的 tuple 清单直接登记支持的 Feature、Storage 和 Command。
Descriptor 不再导出私有定义，登记不创建实例。`run()` 激活已选 Handler，PrepareStorage/Attach 才创建 Storage。
内置路径不执行外部扩展的事务验证或回退；设备能力仍在冷态选择支持项。

外部扩展包含 `sinclude` 的 `runtime/FeatureRegistration.hpp`，声明 `Descriptor`、`Storages`、`Commands`
和 Graph 构建入口。模板验证整个依赖集合后发布到同一目录；失败不留下部分目录，重复登记保留 ID 和当前选择。
具体实现不进入安装公共头。所有声明、回调和类型名都要求定义模块存活至 Server 及相关消息销毁。
扩展只在 `run()` 前或 RenderThread 安全点登记，普通请求不承担注册或实例化工作。

`stop()` 只请求停止，应用仍须 join 渲染线程，并让传入的 ExecutionRuntime 保持存活。
