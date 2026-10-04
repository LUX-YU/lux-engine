# EC3-F：AGENTS、类型复用、复杂度与可读性

## F1. 对“复用类型”的明确理解

复用已有语义类型，不是保留不适合目标的内存布局。CommandDescriptor 原位演进成描述视图、由 Entry 保活，是复用；再创建 CommandSpec/CompiledCommand/DescriptorAdapter 是重复 schema。

相机继续使用原 Camera/Transform 数据；无需为普通导航引入三个 provider。配置值继续使用原 ConfigurationValue／codec；设置文档是来源与持久性，不是新作者数据格式。运行资产读取继续复用 EC2 的 ScriptAbility/Process/VFS，不能被 Editor 设置或命令层接管。

区分数据、准备权限、实际执行、结果观察；不是每个名词都必须有执行者对象。只读查询成员合理，拥有资源的 owner 也应承担控制行为。纯准备算法可以是自由函数。

## F2. 字符串成本与正确性

字符串比较并不天然是项目最大瓶颈，已有 hash 类型也不保证所有查找已经使用 hash。本次已确认 Registry 的 find 仍是线性名称扫描，优先改这个真实路径。

固定命令和工具角色不再运行期按文本解释。合法文本用途继续存在：显示、搜索、文件路径、配置 codec、诊断、冷注册验证以及跨目录版本重新解析。

不要将 `std::string ==` 全局替换成 hash equality；文件相同、字节相同、schema相同、命令相同是不同判据。hash 不是通用数据完整性或权限证明。

没有证据不宣称固定提速倍数。当前专项历史性能来自 P10Q 原 SHA，报告中的深链、选择同步和全快照投影数据不能当成本次 EC3 全产品成绩。[H01]

## F3. 测量五组有限问题

| 编号 | 实际路径 | 必须记录 |
|---|---|---|
| M1 | 35/256/扩展规模的命令注册与查找 | 构建耗时、业务名字 hash/compare 次数、索引容量、lookup 次数、PINNED/CURRENT 场景 |
| M2 | 静态描述、插件静态、动态工具描述 | sizeof、字符存储、Entry/回调/owner/索引分配、菜单重复文本，最终释放 |
| M3 | 稳定菜单与设置页面 | 固定 revision 下有没有重新解析 shortcut/group、重复读取文件、重建全表 |
| M4 | 导航→ECS→已显示输出 | 重复算法、patch 次数、借用／间接调用、相机输出一致性，不把 GPU wall time 当纯 C++ 成本 |
| M5 | Inspector 增量生成 | 干净生成、无变化、单字段／template／support 修改与输出删除时执行的实际 job |

M1/M2 使用真实 SDK 和实际库，不用完全不同的小类证明原实现节约内存。计数段和耗时段分开；计数改动不能冒充正式性能构建。

指定热路径目标：已解析的成功派发不做业务名称比较／重新哈希，不因描述物化复制字符；固定 revision 菜单不重新解析；相机不变时不做多余 patch；未变输入不重复 codegen。

这些目标不包含 handler 自己合理的业务工作，不包含返回错误诊断的字符串构造，不宣称整个进程零分配。

不预设 100 次硬配额或“快 20%”。使用能证明结构变化和覆盖尾部异常的有界样本，记录实际样本数、负载、编译器与条件。不得再次启动被用户停止的旧慢算法长测。

## F4. 过度封装与冗余校验登记

每个改动边界用统一字段：owner、输入、输出、必要不变量、资源寿命、调用频率、回调／线程边界、旧／新依赖、为什么保留或合并。

可删除：无语义转发、无必要类型擦除、同一有效期重复 lookup/describe、同一固定源重复编码、稳定帧无条件全量目录复制。

必须保留：外部输入、插件回调之后、异步完成、Session/History/Run/View 代次、文件真实发布、GPU 可采样与资源退休、设置外部变动后的检查。

不得用永久 ValidatedHandle、assert、忽略错误、无界重试来减少分支。不能以“单线程”否认重入和析构 callback。

## F5. 风格与小项

- 类型 UpperCamelCase；枚举 E 前缀／成员全大写；variant 别名 V 前缀；private 成员后缀下划线；方法小写驼峰。
- 控制变量用准确语义，不用 optional<bool> 表达三种业务动作，不用 label/name 充当身份。
- 长 callback 和 result 使用有意义别名，不为每个 alias 单独建头。
- 多个独立校验按 AGENTS 具名 bool 分组；先检查指针／范围／溢出，再计算依赖它们的条件，不能外提导致提前解引用。
- 数据／组件头只 include 字段类型和标注；template/inline 所需依赖精确保留，不能盲目删除必要 include。
- 只有 CPP 需要的编译器、平台、JSON 解析或渲染重头迁到实现。
- public include 安装，sinclude/pinclude 不安装；生成 support 的使用范围单独列明。
- 类型更名后全部调用和 SDK 同步，不留兼容 using 或自指 using；旧磁盘名称需要保留时由 codec 显式解释，不用 C++ alias 伪装。
- 结构化错误与退出／日志文字分开，不比较文案作业务判断；新错误使用原 error 域，不建全局 ErrorAdapter。
- 内置路径不增加 try/catch；第三方/插件/Builder/Codec/Toolchain 捕获必须在明确边界转换。禁止全局禁异常开关，也不能让错误穿 DLL/Task/Script ABI。
- 日志出口宿主装配一次，render 诊断仍走原 sink，不额外链接日志后端。
- 不为每个错误路径添加 printf，不用 assert 作为 RelWithDebInfo 必须成立的检查。
- 静态 constexpr 是描述，不在全局对象构造中注册。加载和卸载阶段显式可见。

## F6. 构建、SDK 与证据

显式当前阶段 `LUX_EDITOR_MIGRATION_STAGE=EC3` 与 `LUX_EDITOR_LAYERING_MODE=STRICT`。在原门禁中登记阶段及规则，不以 unknown stage 自动跳过旧限制。

最终全量 target all，遵守 `-j 4` 与 `-k 0`；CMake 修改后第二轮应无工作；不得并发构建与真实 DLL/GPU 运行。

Foundation/closure 先通过 ValidateTrackedSnapshot，从干净 tracked commit 建独立配置；实际用增量构建就写增量，不能当作首次全仓冷构建。

modules 公共头变化同步三个指定 include 前缀；若某前缀不可访问，记录具体缺项并确保 meta-gen 不读旧版本，不宣称同步通过。Android 默认不进入 configure/build/CTest。

新安装 SDK 必须独立消费，不能借旧 build DLL、旧生成脚本或源码 private 头。公开模板／元数据引用／ABI／DLL 代码寿命一起验证。

历史快照逐字保留；对原报告的发现新增勘误，不改以前结果。未修改路径可以按旧 SHA 继承，不记为新 SHA 重跑。

不把已免验的 P12 人工菜单观察、归档验证器项目改名为本阶段强制 smoke；本阶段新增设置、命令、相机和生成能力仍需要对应功能验证。

## F7. 文档与最终维护入口

根 README 说明 Application 的实际边界、配置来源、窗口策略和当前 Editor 扩展版本。清除当前文档中 V7/V8/V9 混写，但历史报告版本保持不变。

SDK 给出：模块静态描述注册、动态描述有界存储、冷名字解析与热 handle、无 App 项目保存、插件设置、原生窗口有效配置、两种相机入口和新 Inspector 工具的可用示例。

每个示例真实构建并使用正式 API，不在文档里发明与代码无关的 ConvenienceManager。

最终交接明确新工作区路径、HEAD、用户补丁是否应用、main 是否未改、分支处理是否获授权；不能只写“完成，工作区干净”。
