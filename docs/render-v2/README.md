# Render V2 架构与使用入口

Render V2 的源码位于 `modules/function/render`，通信使用独立的 Platform IPC。
模块按逻辑图、Feature、资源和执行职责组合，不依赖 Engine 或 Editor 产品。

## 模块边界

| 模块 | 职责 |
|---|---|
| core | 基础身份、结构化错误、独占配置值和执行统计 |
| commands | 中立 Command 声明、请求和结果值 |
| graph | 唯一逻辑 Graph、Builder、Analysis 和冷态 PassProgram |
| features | 冷态资源供需解析和单 Graph 装配 |
| resource | 专业资源合同、StorageSet 所有权与 ResourceRegistry 非拥有索引 |
| native/vulkan | DeviceSession、编译、帧资源、批量上传、提交和物理清理 |
| runtime | Server 对象、Command 路由、关联与请求终态 |
| platform/ipc | 进程内拥有型消息、独立双向 SPSC 与来源关联 |

稳定名称用于冷态发现；Command、Handler、Scene、View、Effect、Storage 等运行时身份各有独立身份域。
Scene 拥有 View，Effect 拥有不可变 CompiledGraph，Attachment 拥有 ExecutionContext。
Target 拥有实际输出图像，同一 Target 只有一个 Attachment。
StorageSet 独占专业 Storage；Registry 只定位实例。Client 显式管理资源间的业务依赖。

DeviceSession 独占设备设施和物理 Queue 集合。上传保留具体 stdexec Sender，物理清理只进入
Session 的 `cleanup_scope_`。资源在 Copy/Acquire 接纳、使用顺序成立并完成发布后返回句柄；
实际 GPU 完成决定物理资源何时可以复用或释放。没有对外 Ticket 或生产 Readback Command。

帧槽在参与的物理 Queue 完成且 CPU 写入结束后复用。默认两个在途帧，可配置一到三个。
动态 Storage、Context 和 Target 使用对应物理 slice；CPU 业务数据、Shader 和 Pipeline 保持唯一。
Session 统一关闭输入批次、准备帧资源、合并兼容 Context 的录制并提交。详见
[Native 所有权和执行合同](../../modules/function/render/native/vulkan/README.md)。

## 宿主与 Client

宿主创建一个 Core `ExecutionRuntime`，通过选定的
[`vulkan::createRenderServer`](../../modules/function/render/runtime/include/lux/engine/render/runtime/VulkanServer.hpp)
装配 Server，并在冷态安装 Feature/Storage 提供者。宿主拥有线程；Server 借用同一个 CPU scheduler。
[`RenderServer.hpp`](../../modules/function/render/runtime/include/lux/engine/render/runtime/RenderServer.hpp)
本身不暴露 Vulkan 配置。

Client 直接使用 `IpcRequester` 和 `IpcMessage`，自行生成 RequestId，通过普通目录请求取得 CommandId。
没有单独的 RenderClient 包装。两个 Client 的 RequestId 数值可以相同，回复仍沿原 Link 返回。
发送成功转移消息值；发送失败保留原消息。Response 包含显式 `RenderError error` 和业务字段，
业务字段只在成功时消费。回复不自动返还原 Request。
断连只说明结果无法送达，不保证已接纳的业务被撤销。

上传源是借用：Client 从提交成功到收到 Response 保持源地址、范围和内容有效。
FAILED/CANCELLED 同样在最后一次 CPU 读取结束后产生；abandon 或断连不证明读取已经结束。
没有 Response 时，Host 清偿 Server 和 Producer 后才能释放源。Native 独立管理 Staging 和已提交资源。

IPC 使用同进程、同受控 C++ 构建合同。`get<T>()` 的调用者必须知道消息内的实际类型。
宿主必须保持提供类型操作与回调的模块已加载，直到所有相关值析构及回调栈返回。

Server 同步请求直接结算；异步 Command 的 OperationState 由 `counting_scope + spawn` 管理。
Owner RunLoop 将完成交回 RenderThread，ResponseOutbox 统一发布并处理背压，不重复执行 Handler。
停止时先关闭准入，清偿 CPU/GPU、Owner 事件、Producer 尾栈和回复，再释放 Responder、设备和 Instance。
详见 [Runtime 使用合同](../../modules/function/render/runtime/README.md)。

## 构建与可运行消费者

独立构建入口是 [`cmake/render-v2-bootstrap`](../../cmake/render-v2-bootstrap/CMakeLists.txt)。
需要安装同级 lux-cmake-toolset、lux-cxx，以及 Native/异步路径所需的 Vulkan、stdexec 等依赖。

```sh
cmake -S cmake/render-v2-bootstrap -B ../build/render -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON \
  -DCMAKE_PREFIX_PATH="<toolset-prefix>;<cxx-prefix>;<dependency-prefix>"
cmake --build ../build/render --target all -j 4 -- -k 0
ctest --test-dir ../build/render --output-on-failure -j 1
cmake --install ../build/render --prefix ../install/render
```

`LUX_RENDER_CLIENT_ONLY=ON` 选择同步 Client 所需的中立值与 IPC，不选择 Vulkan 或异步实现。
完整 Engine/Editor 产品接线是独立构建范围，不能用 Render bootstrap 成功替代。

[`render/test`](../../modules/function/render/test/CMakeLists.txt) 保留同步 Client、Native 输出和
Host/Main/Asset 双 Client 的 Mesh/Material 场景。Client 翻译单元不包含 Native 头；测试自行进行
普通 Copy 和映射来检查输出，不在生产 API 中添加 Readback。

[色彩空间约定](color-space.md) 说明采样与工作色转换；
[Legacy 参考边界](LEGACY_MANIFEST.md) 说明冻结源码的用途与校验方式。
阶段报告和历史资格从 Git 历史查询；原始验证输出保存在源码树外。
