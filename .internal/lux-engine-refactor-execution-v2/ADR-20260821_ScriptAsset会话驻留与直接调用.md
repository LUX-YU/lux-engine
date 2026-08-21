# ADR-20260821：Script Asset 会话驻留与直接调用

## 状态

已接受，等待代码施工。本 ADR 取代
`ADR-20260821_ScriptRuntime契约与错误边界.md` 中的 Runtime-validated handle、
invoke-time module lookup 与 stale-handle 调用模型；旧 ADR 作为历史施工记录保留。

## 背景

Script 在 Engine 中是 `ScriptAsset`，已由 `AssetManager` 与 `AssetRef` 表达身份和
驻留需求。当前 Native 事件调用却依次经过 event thunk、两层锁、module/function
哈希查找、`shared_ptr`、虚调用与 `expected`，最后才进入
`lux_script_invoke_fn`。这些工作属于加载和绑定边界，不应每实例每帧重复。

`AssetRef` 当前仍是 UUID 驻留需求票据，不是稳定资产对象指针或 GC root。
全局 Asset 回收、logical invalidation 与 physical destruction 需要独立设计，
不在本阶段通过 Script 私有 lease 或 handle 抢先裁决。

## 裁决

1. `ScriptAsset` 是唯一内容身份；`AssetRef` 继续表达 ScriptComponent 的驻留需求。
2. Lua chunk 和 Native dynamic module 是一次播放会话的派生执行数据。首次绑定后
   驻留到 `SceneScriptRuntime::stop()`，不因 AssetRef 归零或最后实例销毁而卸载。
3. 会话内容是快照：同一 asset id 一旦加载，当前会话继续使用该版本；
   内容变更或删除在下一次会话生效。
4. 名称查找、ABI/manifest 校验、签名匹配、Lua handle 解析和 C++ 类型检测
   只在加载/绑定冷路径执行。
5. 绑定结果是 `{lux_script_invoke_fn, void* context}` 两个指针的纯数据记录。
   ECS 事件热路径直接调用最终函数指针。
6. ScriptEvent 签名必须精确匹配参数数量、kind、size、type id 与顺序；
   不保留参数前缀兼容。
7. `ScriptRuntime`、`ScriptFunctionHandle`、`ScriptFunction`、`IScriptModule`
   与通用 Backend/Module 虚调用层删除。Function Script 只保留 ABI、value/signature
   与具体 move-only `NativeModule` 加载 API。
8. C++ Behavior 基类不再多态；注册模板使用 `requires` 检测 `noexcept`
   生命周期函数并生成直接 ABI thunk。
9. Native/C++ AOT 是可信任进程内代码。删除 Windows SEH CrashGuard；真正不可信
   代码使用 Lua，未来可选 WASM 或进程隔离。
10. `LUX_SCRIPT_ABI_VERSION=1`、C struct 布局、ScriptAsset Schema v2 和全部 wire 不变。

## 会话所有权

进程全局 `ScriptRegistry` 只保留 C++ Behavior 静态注册。Lua/Native backend 由
`SceneScriptRuntime` 显式拥有，虚接口只用于 bind/beginFrame/resetSession 冷路径。
停止顺序固定为：停止派发 → OnDestroy → 销毁实例 → 释放 AssetRef →
移除 ScriptSystem → reset backend 会话缓存。

## 性能契约

- `BoundScriptCall` 在 64 位平台固定为 16 bytes 且 trivially-copyable。
- bulk event 的 value slots 与 call frame 每个 event 只构造一次。
- 成功热循环每 subscriber 只读取两个指针、写 `user_context`、调用一次
  最终函数指针并检查整数返回码。
- 调用期间不得出现 mutex、hash/string lookup、`shared_ptr`、`expected`、
  虚调用、分配或 stale/revision 检查。

## 后续项

独立设计 AssetManager 的显式回收、GC/root、logical invalidation 与 physical destruction。
该项不以引用归零即卸载为默认结论，也不在本 Script 施工中提前完成。
