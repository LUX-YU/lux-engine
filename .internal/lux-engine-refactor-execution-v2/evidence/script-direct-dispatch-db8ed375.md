# Script Asset 会话驻留与直接分派施工证据

## 基线与提交

- 代码基线：`db8ed375`
- 文档优先提交：`d7cd367d`
- 实施提交：`a478e173`
- 分支：`codex/script-direct-dispatch`

## 已完成边界

- `ScriptRuntime`、`ScriptFunctionHandle`、`ScriptFunction`、`IScriptModule`、通用
  Lua/Native Runtime factory 与旧 public headers 已删除，无 alias、shim 或 forwarding header。
- Function `script_core` 只保留 ABI、Signature、Value、非空 `CallFrame` 与加载/绑定期
  `ScriptResult`；`script_native` 以 move-only `NativeModule` 完成 path/memory load、ABI、
  host binding、函数表与重复名称校验。
- ECS 绑定结果统一为 16-byte、trivially-copyable 的 `BoundScriptCall`；Native、Lua、
  C++ Behavior 均在冷路径生成最终 `lux_script_invoke_fn + context`。
- ScriptEvent 对参数 count/kind/size/type ID/顺序执行精确校验，旧参数前缀兼容被明确拒绝。
- C++ `ScriptBehavior` 无 virtual、vptr 或虚析构；注册模板以 `requires` 生成精确
  `noexcept` ABI thunk。
- `SceneScriptRuntime` 独占 Lua/Native backend 与会话缓存；同一 asset id 在一个播放
  会话内使用首次版本，`stop()` 先销毁实例与 AssetRef，再 reset backend 缓存。
- `ScriptCrashGuard`、Windows SEH trampoline/filter 与从 native memory fault 恢复的承诺已删除。
- ScriptAsset Schema v2、Asset wire、`LUX_SCRIPT_ABI_VERSION=1` 与 FlowForge AOT ABI 未修改。

## Owner、生命周期与 ABI 测试

以下 RelWithDebInfo owner executables 均返回 0：

- `script_core_contract_test.exe`
- `script_native_runtime_contract_test.exe`
- `native_script_session_contract_test.exe`
- `script_direct_dispatch_contract_test.exe`
- `script_system_direct_contract_test.exe`
- `script_asset_request_system_test.exe`
- `asset_lifecycle_test.exe`
- `asset_codec_catalog_test.exe`
- `asset_wire_contract_test.exe`
- `asset_load_service_test.exe`
- `schedule_topology_test.exe`
- `scene_roundtrip_smoke.exe`
- `game_export_smoke.exe`
- `flowforge_aot_test.exe`

覆盖内容包括 Native path/memory load、精确签名及漂移拒绝、会话首次版本冻结、reset 后
重载、失败脚本禁用且后续订阅者继续、实例先于 AssetRef 销毁、FlowForge JIT/AOT
differential 与缺失 host import 拒绝。项目根当前未注册 CTest，因此四个 Profile 的
`ctest` 均报告 `No tests were found!!!`；这里以 owner executable 实际运行记录验收，
不把零项 CTest 误报为覆盖。

## 热路径机器契约

- fixture：100,000 个 no-op `BoundScriptCall`，预热后每样本重复 100 次。
- 最新中位结果：生产直接循环 `7,740,500 ns`；raw 基线 `7,614,700 ns`；等价手写
  基线 `7,829,300 ns`；相对等价基线 `0.9887x`。
- 全部成功热循环动态分配计数为 0；绑定后 lookup/load/signature-validation 计数不变。
- MSVC RelWithDebInfo 反汇编显示循环内加载 context、写入 `frame.user_context`、加载
  invoke、执行一次 `call rax`、检查整数结果并前进；成功路径没有 Runtime、virtual、
  mutex、hash/string、shared_ptr、expected 或 stale/revision 符号。

## 构建、安装与门禁

- Windows RelWithDebInfo `DEVELOPER`、`PLAYER`、`EDITOR`、`TOOLCHAIN` 均完成完整
  `target all -j 4 -k 0`；各自第二轮均为 `ninja: no work to do`。
- 配置期 Module Layout、生产 target 分类与 layer/product DAG 门禁通过。
- installed `script_core` consumer 配置、编译、运行通过。
- installed `script_native` consumer 能加载 fixture、查找 descriptor 并直接调用，配置、
  编译、运行通过。
- Debug、RelWithDebInfo、Android 三个 include prefix 均安装 `NativeModule.hpp`，且不存在
  `ScriptRuntime.hpp`、`ScriptBackend.hpp`、`ScriptModule.hpp`、`LuaBackend.hpp`、
  `NativeBackend.hpp` 或 `ScriptCrashGuard.hpp`。

Android PLAYER 的新构建树已进入交叉配置，但 vcpkg 当前没有可用的
`luajit:arm64-android`：其 host `buildvm-64` feature 在 `x64-windows` 被 port 标记为
unsupported。由于 Player 必须保留现有 Lua ScriptAsset kind，本轮没有用宏裁掉 Lua
来制造假通过；该项记录为外部交叉工具链依赖阻塞，Windows 四 Profile 与三个安装头前缀
不受影响。

## 归零扫描

- production/test/CMake 不存在旧 Script Runtime/FunctionHandle/Module、SEH Guard、
  `invoke_mutex`、`guardedScriptCall` 或 `STALE_HANDLE`。
- ECS Script 与 Runtime Scene Script production 不调用同步 `ensureAsset()`。
- 安装树不存在旧 public headers。
- `git diff --check` 通过；用户已有的 Input 私有头格式调整保持未提交。
