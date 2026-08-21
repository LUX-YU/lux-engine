# ADR-20260821：ScriptRuntime 契约与错误边界

## 状态

已接受，代码施工尚未完成。

## 背景

当前 `script_core` 已经形成一个真实的语言执行边界：Lua、Native DLL 以及未来可能存在的
WASM backend 可以同时存在，并由同一个对象完成 backend 注册、module 加载、function 查找与
调用。这种多态对应多个真实实现，不是为搬运平台差异而制造的 Adapter。

现有公共 API 仍有三项问题：

- `kInvalidModule`、`nullptr`、`bool` 与线程局部 `lastError()` 共同表达失败，调用方无法可靠组合错误；
- Native backend 在公共库内部直接写 `stderr`，宿主无法统一诊断出口；
- `ScriptFunctionHandle` 保存 module 内部裸指针，module 卸载后可能悬垂。

## 裁决

1. `ScriptHost` 重命名为 `ScriptRuntime`，旧头、旧类型名和 forwarding alias 不保留。
2. 保留现有 backend 多态；不创建 Adapter、第二套 Runtime、Service Locator 或全局 backend registry。
3. `ScriptRuntime`、`IScriptBackend` 与 `ScriptFunction` 使用同一组结构化错误：
   `EScriptError`、`ScriptFailure` 与 `ScriptResult<T>`。
4. module load、memory load、function lookup、invoke、unload 和 backend 注册均返回 `ScriptResult`；
   删除 `lastError()` 及以 invalid handle/null/bool 作为可诊断失败的公共通道。
5. `ScriptFunctionHandle` 继续作为现有公共概念，但不得保存 module 内部裸指针。
   Runtime 在调用时验证 module/function 身份，并保证调用期间 module 存活；卸载后的旧句柄返回
   `STALE_HANDLE`。
6. 库只返回结构化诊断，不决定文字输出位置。日志、终端和 Editor 面板由宿主装配。
7. `ScriptModule` 只表示语言 Runtime 的加载单元。Native Script ABI 与 Engine Extension ABI
   相互独立，任何 Script target 都不得 include 或 link Extension ABI。
8. `lux_script_abi.h` 的 ABI version、C symbol、结构布局和调用约定保持不变。

## Navigation 与 UI 关联裁决

`navigation_detour3d` 当前已是独立 target/component，Recast/Detour 为 PRIVATE 依赖并具有 owner
test；`FUNC-009` 按事实验收，不再安排搬迁。

旧 UI 章节列出的 `ui_imgui`、`ui_imgui_glfw`、`ui_render_vulkan` 四目标结构不再是强制施工图。
UI 必须在独立 ADR 中按领域所有权重新调查；不得把 GLFW/Vulkan 调用简单包成公共 Adapter target。
`SceneViewportPanel` 归 Editor 与 UI 公共闭包退出平台/渲染 backend 仍是目标，但最终 target 数量与
命名由后续裁决决定。

## 验收

- Lua/Native file、memory、find、invoke、unload owner tests。
- unknown/duplicate backend、坏扩展、坏 ABI、坏入口、缺失函数与 backend invoke failure。
- module 卸载后旧 function handle 明确返回 `STALE_HANDLE`，不访问释放内存。
- Script 库没有 `stderr`、`lastError()`、`ScriptHost` 或公开 `kInvalidModule`。
- installed `script_core` consumer 不获得 Lua、DynamicLibrary 或 Extension ABI。

