# EC3-E：Inspector 生产生成统一到 MetaUnit／inja

## E1. 现状与范围

已读三条核心链：Render pass/operation 和 ScriptAbility 使用原 codegen job／validation／projection；Inspector 先使用同一 parser 的 MetaUnit JSON，再由 Python Generator 拼接 C++。[S31–S34]

Inspector 的 Python 不是仅测试脚本。它处理注解、类型、控件、源码发射、clang-format 和产物更新；其中作者／Run 版本还有生成后全文 replace。此为本轮明确迁移对象，不是因后缀 `.py` 就断言所有脚本不合理。

本轮统一的是生产结构代码的解析／语义／模板链。测试驱动、归档核验、依赖检查、下载、数据统计与 CMake 元配置可以继续用 Python／CMake。shader compiler、反射 parser 和 link 工具不是文本模板器，不改造成 inja。

C0 必须从所有生产 CMake 自定义命令和生成 target 建立完整 producer 清单，记录 input→semantic→emitter→output→consumer→install。已知 3 条链不是全仓统计。

## E2. 目标链路

```text
原 C++ parser 与 MetaUnit
    → 一份结构化 Inspector 语义模型与完整验证
    → inja 模板／片段
    → 作者或 Run 的显式 projection
    → 格式化／输出发布／依赖记录
    → 实际消费者编译与运行
```

禁止第二个 C++ parser、正则替代整个类型系统、Python f-string 生成生产函数体、生成后 `replace('InspectorComponent',...)`，或 C++ 先拼完整 body 再让 inja 仅输出 `{{body}}`。

保留数据／处理者／结果分工：MetaUnit 是输入数据；语义准备处理类型图／字段规则；模板是输出结构；生成产物不是语义权威。

## E3. 实现归属与工具复用

优先扩展已有 lux-cxx codegen 的 projection／validation 能力；通用 parser、MetaUnit、模板引擎不链接 UI、Editor、Runtime 或 GPU。

Inspector 专属语义归一化放 Editor 的构建工具主题。若现有生成器无法挂接所需变换，允许一个窄 host tool 复用同一 MetaUnit/inja 库；不能另立通用 codegen 框架。工具 target 不混入运行库。

模板与 support helpers 归 `editor/workbench/scene/codegen` 的实际 provider。生成代码被相应 Editor target 编译，纯组件头仍只包含字段类型与反射标记。

CMake 继续使用显式 target_files/logical_paths/marker 和原 `lux_*codegen*` 入口。不要把 output 根改成源码目录，不在 repo 中手工维护生成文件。

构建工具必须用 host 架构。若目标平台不能运行 host tool，明确 host 工具路径与版本；本阶段不新增跨平台资格声明，也不能假定所有目标二进制都能在 host 执行。

## E4. 语义模型必须保留的字段能力

从现有 Generator 与 support 代码逐项搬运，不按下表猜测删减：

| 内容 | 必须保持 |
|---|---|
| bool／整数／浮点／枚举 | 精确字段类型、原控件选择、changed 语义 |
| 字符串及固定文本 | 容量、编码和提交约束；不能用未经定义的截断 |
| 嵌套 reflected record | 正确的成员路径与叶子字段，非反射状态不得错误交换 |
| C array／std::array／vector/deque/list | 插入、删除、索引和 readonly 语义 |
| map/unordered_map/set/unordered_set | key 不可非法原地修改，集合操作与历史一致 |
| optional／variant／pair／tuple | 活跃分支、构造销毁、版本和只读控制 |
| Eigen／向量／矩阵／颜色 | 精度、排列、特殊控件和原字段语义 |
| Asset/custom widget | 实际类型与 tag，调用原自定义入口 |
| min/max/speed/step | finite、顺序、正步长、溢出及整数精确范围 |
| readonly | 覆盖完整嵌套控件和结构操作，不能只禁用外层按钮 |
| preview/commit/cancel | 原字段能力、一次领域批次、Undo/Redo |
| paused Run 编辑 | 与作者编辑显式不同接口和准入，不写作者 checkpoint |
| diagnostics | 未识别类型或注解必须失败并定位，不输出可编译但语义降级代码 |

现有 Python 使用 Decimal 等处理时，C++ 迁移不得通过 double 丢失 64-bit 整数边界。使用现有数值解析／codec 或 from_chars 的准确整数路径。不是将所有注解先转 float。

Typedef、elaborated、array 和 canonical type 的归一化应从 MetaUnit 取得确定关系。遇到缺失 IR，修补正确的输入/解析契约；不能静默假造 UnsupportedType 并给默认控件。

## E5. 模板职责

至少分开公共声明、组件实现、作者绑定、Run 绑定；片段可共享。模板参数明确 binding mode、namespace、fields capability、类型路径、只读政策和输出 stem。

差异来自结构化数据，不来自输出文本替换。可以用模板循环／条件表达重复结构，但复杂类型图遍历、循环检测和数值校验应在语义阶段完成。

合法的 C++／字符串字面量转义可用共享 callback；不能因为保留 inja 就再次手写多份转义算法。UTF-8、引号、反斜杠、换行和异常注解都要有实际负例。

模板文件名沿原工程 convention，`.template` 不必为了统一再改成 `.inja`。选择引擎和输入契约比扩展名重要。

固定格式只保留一份 style 配置。不要同时让 CMake、Python、C++ 各自内嵌一段不同的 clang-format JSON。

## E6. 输出与增量构建

声明全部 OUTPUT／BYPRODUCT：hpp、cpp、MetaUnit JSON sidecar、依赖文件、schema 或列表文件；不要只声明一个空 marker 再从旁边偷读真正变化的 JSON。

优先使用原 parser 真实 depfile。当前递归依赖 modules/engine 全部公共头是保守方案；只有真实传递依赖覆盖后才能收窄。删除广泛依赖不是自动正确的优化。

生成 input 配置、编译 flags、include 路径、宏、logical path、template、support helper、formatter 及生成器版本的变化，都要使正确步骤失效。

语义检查和模板渲染成功后再发布输出；内容不变不改时间戳。多输出全部预生成到 staging，失败不让后续编译使用新旧混合的“成功”集合。文件系统不能提供整组原子替换时，生成步骤失败且不发布成功 stamp；下次确定性重建，不声称实现了跨文件原子事务。

某个 sidecar／生成 CPP 单独被删除必须重建；不能因为 marker 未变继续使用旧文本。多个任务同名 stem 必须按 logical path／原唯一规则区分，不发生覆盖。

显式输入顺序或稳定排序使输出确定，不依赖目录遍历随机顺序、机器绝对路径、时间或 locale。路径含空格／中文应实际验证，不能只写文档支持。

统一工具迁移后同步删除 `inspector_codegen.py` 的生产调用、旧 string emitter、只服务旧脚本的 CLI 和安装文件。旧冻结 dev_log 中的脚本保持原样；测试程序可迁移调用新工具，不删除原语义断言。

## E7. 三条原生成链的边界

Render 原有七个模板文件和 ScriptAbility 的 C++/schema/Lua/native/validation projection 是复用对象，不因本轮统一重新改 ABI 或输出语义。

如果公共 codegen 修改影响它们，必须跑对应生成／编译／实际消费回归。没有变动时按准确 SHA 继承已有证据，不声称本轮重新验证全部 shader 或脚本引擎。

生产 helper 中出现 `file(GENERATE)`、配置 `.in` 或少量版本 header，不一概判成违规；逐个说明它是构建元数据、嵌入资源、语义代码还是测试夹具。不要为了数字“全部统一”把适合 CMake 的工作强行迁入 inja。

## E8. 安装消费者

新的 SDK consumer 只能使用安装的 generator、模板、support、组件头和公开 CMake 函数；不能读取源码树 pinclude、旧 build DLL 或通过源码相对路径找到 Python 脚本。

模板里可以生成 ImGui 调用，但 generator 本体不必链接 ImGui；组件 target 和运行数据 provider 不反向链接 Editor。构建工具、模板和 support 安装闭包分开列明。

修改 modules 公共头后，同步 AGENTS 指定 Debug/RelWithDebInfo/Android include 以避免 parser 读旧头；这只是头同步，不计 Android build PASS。

在实际 C++20、项目 MSVC/STL 和 codegen 输入下验证。不能只给一份本容器编译的小示例就替代安装 Inspector 的真实编辑行为。

## E9. 本章验收

完整语义正例、非法注解／类型负例、author/Run 两条真实字段能力、输出缺失重建、无关头变动不重建、相关头与 support/template 变化正确重建、第二轮无工作、安装 consumer、路径搬迁和重复 stem。

对旧／新生成器的比较以行为与稳定符号为主；格式差异可以存在，不把纯逐字节相等当作唯一标准。改 hash/stem 策略需同步消费者与删除旧产物，防止重复定义。

永久只保留一个当前生产 emitter。迁移期可在开发夹具中比较旧版本，但最终产品构建不能保留 `USE_PYTHON_GENERATOR` 回落开关。
