# Render V2 架构与使用入口

Render V2 的源码位于 `modules/function/render`，通信使用独立的 Platform IPC。
模块按数据、逻辑图、Feature、资源和执行职责组合，不依赖 Engine 或 Editor 产品。

## 模块边界

| 模块 | 职责 |
|---|---|
| core / data | 基础身份、错误、持久数据编码与拥有型字节 |
| protocol / commands | 中立 Command 声明、请求和结果值 |
| graph | 唯一逻辑 Graph、Builder 与 Analysis |
| features | 冷态资源供需解析和单 Graph 装配 |
| resource | 专业资源合同、StorageSet 所有权与 ResourceRegistry 非拥有索引 |
| native/vulkan | DeviceSession、编译、执行、Transfer、真实 GPU Ticket 与退休 |
| runtime | Server 对象、Command 路由、关联与请求终态 |
| platform/ipc | 进程内拥有型消息、独立双向 SPSC 与来源关联 |

稳定名称用于冷态发现；Command、Handler、Scene、View、Effect、Storage 等运行时身份各有独立身份域。
Scene 拥有 View。Effect 拥有不可变 CompiledGraph 和执行关联；不同 View 的可变执行资源彼此隔离。
唯一 DeviceSession 管理设备资源。GPU 资源安全性由真实 SubmissionTicket 和多 Queue LastUse 决定。

## 宿主与 Client

宿主创建一个 Core `ExecutionRuntime`，通过选定的
[`vulkan::createRenderServer`](../../modules/function/render/runtime/include/lux/engine/render/runtime/VulkanServer.hpp)
装配 Server，并在冷态安装 Feature/Storage 提供者。宿主拥有线程；Server 借用同一个 CPU scheduler。
[`RenderServer.hpp`](../../modules/function/render/runtime/include/lux/engine/render/runtime/RenderServer.hpp)
本身不暴露 Vulkan 配置。

Client 直接使用 `IpcRequester` 和 `IpcMessage`，自行生成 RequestId，通过普通目录请求取得 CommandId。
没有单独的 RenderClient 包装。两个 Client 的 RequestId 数值可以相同，回复仍沿原 Link 返回。
发送成功转移输入所有权；发送失败保留原消息。业务结果通过独立回复返回，不自动返还原 Request。
断连只说明结果无法送达，不保证已接纳的业务被撤销。

IPC 使用同进程、同受控 C++ 构建合同。`get<T>()` 的调用者必须知道消息内的实际类型。
宿主必须保持提供类型操作与回调的模块已加载，直到所有相关值析构及回调栈返回。

Server 同步请求直接结算；真正异步工作先准备稳定完成落点，再接纳不可回滚的副作用。
CPU 完成和 GPU Ticket 完成回到 Owner 线程结算。回复发布失败保留实际结果并重试发布，不重复执行 Handler。
停止时先关闭准入、清偿 CPU/GPU 工作及所有 ReceivedRequest，再释放 Responder、设备和 Instance。

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

[`runtime/test/basic_mesh`](../../modules/function/render/runtime/test/basic_mesh/README.md)
提供实际 Host/Main/Asset、Mesh/Material、Camera、两 View 和 Offscreen Readback 消费者；
Client 翻译单元不包含 Native 头。该目录也可针对已安装 SDK 独立配置。

[色彩空间约定](color-space.md) 说明采样与工作色转换；
[Legacy 参考边界](LEGACY_MANIFEST.md) 说明冻结源码的用途与校验方式。
