# P02：独立 Scene 作者模型验收

**状态：PASS，仅 P02；停在 P02 等待复审，不进入 P03。**

输入提交为 `3bc49e44646df30ea73990864658a9121789757a`，实现提交为
`fb55a5a9694831242e537a5cb1b3f7080a6f25fe`。实现与本目录验收记录分开提交；分支为 `codex/editor-redesign-v4`，不修改 main。
本目录是冻结快照，唯一可变施工材料仍为 `.internal/editor-redesign/`。

## 实际交付与唯一所有者

- `SessionStore → SceneSession → SceneSource + EditHistory` 是唯一拥有链。SessionState 继续唯一维护
  来源绑定、保存 checkpoint、准入和观察版本。currentContent 私有、无分配，从真实 History 读取；
  不调用 describe，不保存第二份 current、dirty 或 busy。
- SceneSource 的私有 CPU Registry 保存已登记作者组件；对象表仅保存 UUID、分区和未知 payload。
  WorldObjectId 沿用原 UUID。对外只有同步只读借用及拥有编码值，不暴露可写 Registry。
- 结构批次先准备完整 CPU 候选，成功后交换；字段只解码受影响组件，配置只准备正式描述。
  一次批次对应一次既有 EditHistory 提交，undo/redo 没有第二份算法。
- 模型插入接收已加载 ModelAsset，相机创建接收参数值；新模型没有 IO、Root、Runtime、GPU、
  ProjectStorage 实现或旧 Context。Scene/World/Simulation 正式描述都保留。
- capture 同步编码已知组件、复制未知及包/卷字节、克隆描述；不依赖外层 const shared_ptr。
  插件代码寿命长于源值、memento、快照及 deleter。私有重载准备源和新 History，采用前复查内容戳。
- changesSince 有记录数及字节上限；裁剪、历史换代和非法游标明确返回 RESET_REQUIRED。

## 迁出、删除和暂留

| 原实现 | 当前唯一实现及原调用处理 |
| --- | --- |
| FieldEdit.hpp 中 TFieldValue / addFieldBytes | 迁入 model/FieldValue.hpp；原定义删除，旧 Registry 字段交互复用 |
| ModelCreation.cpp 模型展开、TRS 和 UUID 规划 | SceneAlgorithms.cpp；旧文件仅保留项目引用核验和运行内容适配 |
| 组件编码、相机参数到组件值 | encodeSceneValue / makeSceneCameraObject；旧视口仅提取参数及适配结果 |
| ParentEdit 父环遍历 | createsParentCycle；旧侧仅取得 Registry 父关系和驻留/安全点验证 |
| ObjectContent.parent | 删除未使用的重复身份字段及填充代码；Parent codec 仍是权威 |
| 旧 detail::SceneFieldEdit | 明确改为 RegistryFieldEdit，消除与新 PreparedEdit 的同名冲突；不留别名 |
| scene_render 中 Camera / codec | 迁入纯 CPU scene_camera STATIC target；逻辑 include、schema 名和磁盘版本不变 |
| WorldMaterializer 隐式 composition 依赖 | 删除；真正的 WorldLoadingSystem 消费者明确链接 composition |
| 四条传递内部头路径 | asset/world_storage/hierarchy 改为实现私有；删除不存在的 core/sinclude；真正的 codec 消费者显式 PRIVATE 使用 |

SceneEditor/Impl、SceneContent、ObjectEdit/ParentEdit 的运行驻留及选择接线、旧 SceneEditing、
以及原 LegacyPersistenceState 私有桥仍供旧产品使用。新模型不依赖它们；逐项消费者及 **P12 删除期限**
见 migration-ledger.json 的 D0001–D0028、p02_retained_adapters。原 P01 到期项继续保持删除，
本轮没有新过渡桥，没有将整个旧实现复制为新模型。

到期要求“P02 提取纯算法”与“P12 删除旧 Registry 适配壳”分开记录；旧窗口未切换不被写成已经迁移完成。
错误枚举集中于轻量 SceneEditError.hpp，旧公开字段交互头不再拖入完整包/算法头，旧 domain_code 保持。

## 验收证据

| 场景 | 观察点与归档 |
| --- | --- |
| X02-01 | model-content.log：真实 CPU 创建/删除/重挂父/组件增删/字段/配置/模型/相机/undo/redo；保存基线、未知及辅助包字节冻结 |
| X02-02 | model-atomic.log：第三个对象预算失败定位到该 UUID；父环、缺 schema、错字段类型和历史预算失败均不改变全部源、历史、观察版本、dirty |
| X02-03 | model-plugin.log：可变插件节点在捕获后继续改变，旧快照不变；自定义 deleter 检查代码有效；与其它组件无关的字段编辑不解码该插件节点 |
| X02-04 | model-identity.log：不同根 AssetId 下同 UUID；跨 Session/History 引用拒绝；重载后旧引用失效，作者身份不依赖 Entity |
| X02-05 | model-changes.log：数量裁剪、字节溢出、HistoryId 换代返回 RESET_REQUIRED；过期重载候选不采用 |
| P01/R1 | 原 37 个 CTest 名称及断言保留；X01-01～06、关闭 describe 异常/无分配内容戳、门禁负例和证据搬运回归全部运行 |
| 依赖负例 | evidence/model-boundaries.json：直接 runtime、中间 UI/Context、imported/LINK_ONLY、旧 Editor/桥头、未知库均被正确规则拒绝；同一夹具修复后成功 |
| 旧产品 | 原 Scene/Material/Flow 保存与 UI/运行回归、插件及 GPU 测试继续运行；未缩减原测试语义 |
| 安装消费者 | SDK 重新安装；五组原消费者及新的 scene-model 消费者重新配置/构建/运行；独立模型消费者只使用安装 SDK |

完整 `all -j 4 -- -k 0` 成功，第二轮 `ninja: no work to do`。完整 CTest **43/43**，随后顺序执行
六组、七项安装消费者测试；各组第二轮均无工作。数量只作索引，语义对应关系见 receipt.json。
测试构建与运行不并发，使用明确的 MSVC 开发环境。

显式使用 `LUX_EDITOR_MIGRATION_STAGE=P02`，并运行实际图/编译参数检查和原 V4 `--stage P02` 审计。
ValidateTrackedSnapshot 在起点不存在，本轮补入最小脚本；从干净实现提交建立独立 clone，完成配置和
实际闭包检查。该证据不冒充在 clone 中又做了一次全量引擎构建。
无 modules 公共头改动；三安装前缀头同步条件未触发。SDK 已重装，不做 Android 验证。

运行 `python dev_log/P02/check_receipt.py` 验证冻结证据。它只打开 archive_path/archive_log，生产机器
绝对路径仅作为命令来源。缺失或哈希不符立即失败，不跳过；原 P01 inventory/AST 验证器仍复用。
新增、修改、删除文件及 Git 内容哈希见 files.json；全部冻结产物哈希见 artifacts.json。

## 失败证据与限制

开发期构建及测试失败保留于 logs/development：CPU target 接线和生成器路径、
局部名称冲突、一次 MSVC COMDAT 链接失败，以及首次完整回归的内部 include 泄漏/MSVC 环境缺失。
安装消费者曾因未把已安装 vcpkg 的 glfw3/nfd DLL 目录加入运行搜索路径而超时，
同一实现和同一断言补齐明确的 SDK/第三方运行目录后重跑；不借用引擎构建目录的 DLL。
干净 clone 首次配置缺少现有独立 Lua SDK 的包目录，补传同一已安装 SDK 路径后通过；未复制本地源码补齐输入。
辅助包字节测试最初误取了原包第一个成员，已改为定位新增的最后一个成员；冻结前后的值断言仍保留。
最终结果以固定实现提交上的 logs 为准，不用早期通过结果冒充最终验收。

| 既有缺陷 | 本轮原判定 | 后续责任 |
| --- | --- | --- |
| C01 | FAIL，退出 1；布局拒绝后 visible 仍变化 | P09/P12 |
| C03 | FAIL，退出 1；QUERY 内代码过早释放 | P11 |
| C04 | FAIL，退出 1；连接失败仍构造成功 | P12 |

三项复现源码和断言均未修改，没有包装成通过。P02 必测无缺项。

保留限制：索引 World 的内容编辑及分区/索引/存储结构修改准确拒绝；未知引用阻止不安全删除。
结构编辑暂时准备完整 CPU 候选，不能声称零复制或大世界性能已经优化；字段仅处理局部组件。
快照预算是内容/容器/配置存储的计量，不是分配器额外开销、插件模块 RSS 的硬上限。
Q01/Q09/Q11/Q22/Q26 按 Scene 作者域范围报告；未来 Material/Flow 作者域、异步保存、RunStore、
投影和选择交互不被标为完成。没有实施 P03，没有执行本阶段不涉及的人工 IME 或跨平台矩阵。


交付时工作区保留一处验收后出现的 `SceneObjectEdit.cpp` lambda 排版改动，未纳入实现或验收提交。
验收绑定上述实现 SHA；该工作区改动的差异及哈希记录在 receipt.json 的 delivery_worktree 中。
