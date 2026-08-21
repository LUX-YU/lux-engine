# ScriptRuntime 契约施工证据

## 基线与提交

- 代码基线：`de11c05c`
- 文档优先提交：`63aaf270`
- 实施提交：`c792c816`
- 分支：`codex/script-runtime-contract`

## 已完成边界

- `ScriptHost`、`kInvalidModule`、`lastError()` 与旧头/源码已删除，无 alias 或 forwarding header。
- `ScriptRuntime`、`IScriptBackend`、`ScriptFunction` 使用统一 `ScriptResult<T>`。
- `ScriptFunctionHandle` 不保存 module 内裸指针；unload 后 invoke/signature 返回 `STALE_HANDLE`。
- invoke 期间以 shared module state 保证动态库生命周期，并以 module-local mutex 串行化 backend 调用。
- Native backend 校验入口、ABI、module/function table、重复函数名、host bind 与 invoke return code；库内不写终端。
- Lua backend 的 file/memory compile 与 invoke 错误进入结构化 detail。
- Native Script ABI version、C symbol 与结构布局未修改；Script targets 不依赖 Extension ABI。

## Owner 与集成测试

- `script_runtime_contract_test.exe`：注册冲突、扩展分派、file/memory、missing function、重复 module、unload/stale、并发 invoke/unload 生命周期。
- `script_lua_runtime_contract_test.exe`：file/memory load、invoke、语法错误 detail、missing file、unload/stale。
- `script_native_runtime_contract_test.exe`：path/memory、invoke failure、坏 ABI、坏/缺失入口、host bind failure、missing file、unload/stale。
- `script_asset_request_system_test.exe`：Runtime Script asset request 回归。
- `flowforge_aot_test.exe`：JIT/AOT differential、Native ScriptRuntime 动态加载/调用与缺失 host import 拒绝。

上述 owner executables 均返回 0。项目根当前未注册 CTest，因此未把 0 项 CTest 误报为覆盖。

## 构建与安装

- Windows RelWithDebInfo DEVELOPER：完整 `target all -j 4 -k 0` 通过；第二轮 `ninja: no work to do`。
- Windows RelWithDebInfo PLAYER：完整 `target all -j 4 -k 0` 通过；第二轮 no-op。
- Windows RelWithDebInfo TOOLCHAIN：完整 440/440 通过；第二轮 no-op。
- installed `script_core` consumer 配置、编译、运行通过；导入闭包只报告 `lux-engine-function-script_core` 与 `lux-cxx-compile_time`。
- Debug、RelWithDebInfo、Android 安装前缀已同步公共头；三个 include tree 均不存在 `ScriptHost.hpp`。
- NDK 28.2、`aarch64-none-linux-android28` 对 `ScriptRuntime.cpp` compile-only 通过。

## 归零扫描

- production/test/CMake 不存在 `ScriptHost`、`kInvalidModule` 或 `lastError()`。
- `modules/function/script` production 不存在 `fprintf(stderr)`、`std::cerr` 或 `std::cout`。
- `modules/function/script` 不 include/link Extension ABI。
- `git diff --check` 通过。
