# 05　行为验收与工程验证

## 1. 口径

本文件的编号是**验收主题，不是新建可执行文件或测试框架的配额**。优先复用原 CTest/安装消费者与真正的生产实现。验证绑定准确源码、构建与依赖版本。

EC1 报告的 222 回归、13 PLAYER、24 组 SDK、45 公共头只是基线索引；改动以后按真实行为/头/闭包映射。不得为保持数字保留已删除空测试，也不得缩掉断言后凭数量宣称更强。

测试种类分别标记：静态源码检查、编译正/负例、真实 SDK、真实 Process、真实 Lua、PLAYER、GPU/输入。任何一种不得冒充另一种。

## 2. 职责迁移验收

| ID | 操作与输入 | 必须观察到 |
|---|---|---|
| XEC2-01 | 准备失败、重复发布、计划移动构造/赋值、自移动按声明策略 | 不产生半占用；原 publishing 权威唯一；不重复释放、不过早唤醒。 |
| XEC2-02 | worker 读取 plan，owner 放弃未启动或取消已启动发布 | worker 无 ProjectStorage 指针/解除权限；接受任务仍结清；move-only 负例成立。 |
| XEC2-03 | 正确 plan + 错误 receipt/版本/包身份 | 拒绝采用；目录/source/manifest 不被错配数据改写。 |
| XEC2-04 | 文件成功、manifest 失败/Unknown、随后重试/明确放弃 | 已发布事实可查；不重复写已成功文件；不把 Unknown 记失败并回收 lane。 |
| XEC2-05 | 同一配置通过纯 draft 和实际 UI 生成 | 正式编码/系统/绑定/关系相同；纯消费者无 Root/ImGui。 |
| XEC2-06 | 非法 provider/依赖/配置版本、不支持 partition、未知配置 | 原准确错误；无半源/半 UI；unknown bytes/version 保留；不新增此前不支持的 World 编辑声明。 |
| XEC2-07 | GUI/模板/独立插件分别调用配置准备 | 同一实际算法；内置没有私有分支特权。 |
| XEC2-08 | 内置和外部 DerivedArtifact 无窗口发布 | 同一活动；App 不识别具体产物；source/checkpoint/dirty 不被发布改变。 |
| XEC2-09 | 编译后源继续保存，产物晚发布再登记 | 新源保存事实不被旧编译输入倒退；产物保留自身来源。 |
| XEC2-10 | 独立构造 ResultsView/WorkspaceView，销毁一窗后继续活动 | 不含 AppImpl；只观察原结果；UI 消失不销毁其未拥有任务；动作仍走原安全点。 |
| XEC2-11 | 一个编译结果给两个预览，改变一方期望/配方后乱序交付 | 产物不绑定单一 target；两个采用域独立；陈旧结果不能冒充新目标。 |
| XEC2-12 | 默认球体/第二个既有网格配方，失败资源/背压/关闭 | 同一 live preview 算法；最后成功内容保持原版本；资源在真实退休后释放。 |
| XEC2-13 | 请求普通编译失败、预览创建失败、导航临时不可用 | 错误归属准确；不将 preview 错误归 compiler；不丢 native 原因。 |
| XEC2-14 | 查询发布状态、显式刷新布局、多个旧快照、中断重试 | 状态查询不触发目录 IO；selected 恢复范围正确；marker/用户修改/外部版本检查保持。 |
| XEC2-15 | 模型 recipe 编解码、缺依赖、非法路径/摘要、重导入 | 无 IO 的 codec 与真实加载一致；原 ModelCooker 仍唯一；不自动截断或忽略源错误。 |

## 3. 游戏脚本核心验收

| ID | 操作与输入 | 必须观察到 |
|---|---|---|
| XEC2-16 | 纯 C++ 安装消费者走原 AssetReadPort/loadAsset | 真正读取/解码，结果正确；不链接 Editor/Lua/GPU/LLVM。 |
| XEC2-17 | 实际 Lua 通过 ScriptSystem 调 readAsset/typed read | 通过生成/准备/调用/awaitable/恢复整链；不是 C++ 直接调用 callback 冒充脚本。 |
| XEC2-18 | 存在/缺失/类型错误/格式损坏/IO 失败/额度超限 | 分清接纳错误和已接纳业务 outcome；脚本能处理普通 NOT_FOUND 后继续；无异常跨 ABI。 |
| XEC2-19 | 构造大于 2^53 的域/序号，完整 AssetId，非法句柄输入 | 精度不丢；全域/代次/type 校验；不接受伪造 pointer/table/caller。 |
| XEC2-20 | 最小容量 1/2 的并发请求，拒绝后再释放和重试 | 接纳前预算生效；接受工作完成不丢；容量恢复，无静默无限扩容。 |
| XEC2-21 | start 内同步完成与正常后台完成 | 都只走合法完成/恢复点；不在 provider callback 中重入 VM。 |
| XEC2-22 | script stop before IO / after IO before completion / after completion before resume | 三时序资源与额度均结清；无新业务/晚到跨代次采用；不只测 completion.active 一处。 |
| XEC2-23 | 句柄赋值别名、重复 release、槽复用、跨实例访问 | 合同明确；释放失效所有旧别名；不能访问新实例/新结果；没有第二释放。 |
| XEC2-24 | 忘记显式 release 后卸载脚本；满表情况下再创建新实例 | 原生 scope 清理全部结果；不依靠 VM GC/窗口消失；旧域不复活。 |
| XEC2-25 | 已加载数据读取/范围读取：offset+count 溢出、越界、零长度、过大输出 | 返回准确结果；不 IO；拷贝范围和预算明确，不整包 Lua string 化。 |
| XEC2-26 | 资产读完成后 await 原 Delay 再读/写显式开放组件 | 原 ScriptSystem 和 DeferredScriptHost 命令屏障工作；跨 await 不持组件地址；运行修改不进入作者 History。 |
| XEC2-27 | 能力未注入、重复/冲突 provider、错误 schema/type/value codec | 在 prepare/call 对应边界拒绝；缺能力不回落到 Editor 或全局查找。 |
| XEC2-28 | 编译一个新的独立能力贡献，不改通用 backend/core | 复用正常生成/投影；没有 AssetType switch 进入 Lua VM；原 Delay 不被改造为资产特殊路径。 |
| XEC2-29 | 真实插件 codec/value/结果 deleter，脚本停止与最后 owner 释放 | 代码 pin 覆盖最后值析构；非平凡值不冒充 raw resume payload；线程正确。 |
| XEC2-30 | Lua 错误参数/运行错误/正常挂起，typed worker 含可观察 RAII | 继续沿 LuaBoundary；析构完成后才 error/yield；不新增热路径 try/catch。 |
| XEC2-31 | 正式 PLAYER 运行同一资产脚本 | 包/VFS来源正确；无 Editor 编译及运行依赖；正常退出并清空新请求/结果。 |
| XEC2-32 | Editor Run 同一脚本，多视口/关闭一窗/停止 Run | 同一 engine provider；运行与作者分离；没有每个视图一套脚本 owner。 |

XEC2-29 要求的是本轮新增结果/绑定的真实动态寿命；不等于要求任意已加载插件可随时热卸载。若原引擎合同要求阻止或延迟卸载，按原合同验证，而不是强卸代码。

## 4. 工程和性能验收

| ID | 必须执行的检查 | 不可替代的观察 |
|---|---|---|
| XEC2-33 | 原行为映射与 EC1 骨骼 HEADLESS/WINDOW/APP | 删除/修改后原能力仍成立，History/开放路由/恢复/SaveAs 不退化。 |
| XEC2-34 | 真实 CMake/源码/安装负例及对应修复正例 | 在指定错误依赖上拒绝；缺第三方库不能算门禁有效。 |
| XEC2-35 | 公共头与真实 SDK、modules 三 include 前缀、生成输入重建 | 不借旧头/旧 DLL；native headers 不含 Lua 或 Editor；删除入口不留在安装树。 |
| XEC2-36 | 本轮性能/调用计数 | 同一资产不被绑定层再读/再解码；query 无 IO；稳定帧无无条件全目录复制；scope 清理后容量可复用。 |
| XEC2-37 | 全量构建、二次无工作、PLAYER、受影响 GPU/原生输入 | 修改正式消费者后真实运行；不从 build PASS 推导视觉/生命周期通过。 |
| XEC2-38 | 最终源绑定、免验/未测分栏、残留清单、归档 | 记录真实已执行与未执行；不改历史结论，不依赖另一工作树隐藏源码。 |

## 5. 必需依赖负例

至少覆盖下列**真实边**，可放入现有负例框架，不要求新建十四个项目：

1. 游戏资产能力直接或经 INTERFACE/LINK_ONLY 依赖 Editor 的 project/persistence。
2. PLAYER 的脚本运行链含 Editor headers/source/DLL。
3. 纯配置准备包含 SceneConfigurationElement、Pane 或 ImGui。
4. Runtime 资产读取头包含 lua.h 或作者 Session。
5. 通用 ScriptAbility/Lua backend 依赖 Skeleton/Material 的具体资产/编译器。
6. native script assets consumer 必须链接 Lua 才能构建。
7. Lua asset 投影直接打开文件或另有私有线程池。
8. 通用工作台面板包含 ApplicationImpl 或重新开放全量 IApplicationAccess。
9. typed data/component 头 include 行为 system/feature/binding 头。
10. raw memcpy 非平凡对象到外部 async resume（实际编译 negative）。
11. 资产调用使用 PreparedLocalAsyncStart 私有 timer 授权（编译/运行 negative）。
12. 新插件需改变宿主资产 enum 或通用 VM switch 才能接入。
13. 已删除旧公开名仍在 generated/install 对应映射提供。
14. 错误 world/instance generation/capability schema 的调用被接受（真实 runtime negative）。

## 6. 性能证据的最低实用内容

本轮追踪的是新边界，不重做 P10Q 长测：

- 固定依赖/构建模式/输入；同进程 warm path 与 IO path 分开。
- C++ 原读取和 Lua 同调用使用同一 provider；记录实际 read 次数、decode 次数、运输字节与结果保留量。
- Lua 方法准备次数、执行次数、awaitable/continuation 和 native handles 的 high water；复用原 ScriptRuntimeStats/LuaScriptBackendStats。[C16,C20]
- query 方法是否有 IO、taskInfos/snapshot 大数组、每调用字符串方法搜索或重复结构校验。
- 空闲/已取得结果反复查询，未要求绝对零分配；任何新增 per-call heap 必须解释或移至冷准备。
- 小规模与代表性规模各保留可复现样本和算法计数。样本不足不夸大 p99/统计显著；无需固定 100 次。
- 不并发跑 build/安装/性能/GPU。不要把进程启动时间当热调用开销。

容量与生命周期验证是必做，精确 wall-clock 排名不是阶段目的。若没有性能收益也可以保留正确实现，但不能隐瞒显著回退或未结束资源。

## 7. 验证顺序

先原生/纯值/编译负例 → 真实 IO 与脚本 → 安装 SDK/PLAYER → GPU/原生输入 → 最终源与安装残留核对。每批可跑受影响子集，最终以同一实现完成全部本轮适用观察。

全量构建遵守 AGENTS；新增脚本接口需要真实 Lua/ScriptSystem 运行，不要求新增 Linux、IME、Android、sanitizer 全矩阵。若现环境无法进行本轮必需的 PLAYER/Lua 测试，诚实记 PARTIAL，不拿历史免验替代。
