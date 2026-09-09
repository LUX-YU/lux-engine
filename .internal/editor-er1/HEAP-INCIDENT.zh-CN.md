# Clean clone 图像生命周期失败调查

ER-1 资格仍未通过。本记录保留失败与修复的区别，不将重跑通过覆盖原始失败。

## 原始失败

提交 `62b08eba570a5f112753384efca01f2ca9781bb4` 的独立 clean clone，普通 RelWithDebInfo、诊断关闭：

- `E:/lux-er1/q1/raw/gpu-image_lifetime.log`：进程 50636 以 `0xc0000005` 退出。dump 为 `E:/lux-er1/q1/image-lifetime-crash.dmp`。
- `image-lifetime-crash-stack.log` 在 Scene 析构、EnTT LightRenderState sparse pages 释放时检测到损坏。这只是检测点，不能据此认定 Scene/EnTT 是根因。
- `gpu-image_lifetime-02.log` 单次重跑通过；资格仍判失败。
- CDB 默认调试堆三次报告 freed block 后写。最初根据执行顺序怀疑 Pane/快照路径；随后 `trace-frees.log` 将损坏块定位到 `ReflectionRegistry::registerClass` 释放的描述对象。

## 首次非法访问

`E:/lux-er1/evidence/raw/gpu-asan-image-lifetime-05.log` 的 AddressSanitizer 报告完整的分配、释放、非法写入调用栈：

1. 宿主 `ReflectionRegistry::initRegistry()` 已消费生成模块的待注册队列。
2. 新示例 `buildDevelopmentSceneMeta()` 再次调用 `LuxRegisterAllMetas_META()`。
3. 生成回调先将新 `RefClass::type` 地址放入 fix-list，然后尝试注册相同类型。
4. Registry 保留已有类型，释放重复传入的 `RefClass`；fix-list 仍指向释放区域。
5. `meta_register_qual_type_index_fix()` 向该区域偏移 80 字节处写入 8 字节。

此次修复位于新示例组合入口，不改 Scene、Script、Meta runtime 或第三方实现：拒绝未初始化 Registry，然后调用增量 `drainPending()`；去掉直接重放整组生成注册函数。源码提交 `03f7996ea978d0b94d2984bc63d938dbd16e68e9`。

## 修复验证及诊断限制

- `metadata-asan-fixed.log`：未初始化时结构化拒绝，连续 100 次构建保持同一反射身份。
- `gpu-asan-fixed-1.log` 至 `gpu-asan-fixed-5.log`：五个独立进程执行原 image_lifetime 用例通过。
- 此次 ASan 使用现有 MSVC、同一 ImGui 源码的独立插桩构建，没有升级或改动依赖源码，也没有安装到用户已有前缀。
- 为定位非法内存访问，运行参数关闭了两类标记检查：`alloc_dealloc_mismatch=0`（专用故障分配器跨 DLL 的 new/malloc 标记不同）与 `detect_container_overflow=0`（部分 DLL 插桩时 STL 注解不完整）。因此这批结果只证明所覆盖路径未再报告 ASan 非法访问，不作为容器边界或分配/释放类别的完整资格。
- 上述标记检查开启时的两份失败日志 `gpu-asan-image-lifetime-01.log`、`-02.log` 均保留。插桩未覆盖 Meta runtime 时的通过也不算根因修复证明。
- ASan DLL 与构建身份记录在 `evidence/continuation/asan-fix-identities.json` 和 `asan-fix-build-identities.json`。正常 SDK 与成本产物禁止此插桩。

## 后续正常资格失败

`E:/lux-er1/q2` 从 `03f7996e` 独立克隆。脚本将现有 libclang 的目录置于 PATH 前端后，未显式指定编译器，CMake 自动选择 GNU 命令行模式的 clang++，偏离计划中的 MSVC。全量构建失败，未运行该构建的 EXE；原始配置及构建日志保留。资格脚本正在修正为显式传入 MSVC cl.exe 的完整路径。

正常 SDK、迁移消费者及剩余阶段门槛未因上述诊断通过而自动获得资格。
