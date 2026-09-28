# P03：独立 Material 作者图验收

状态：**PASS（P03 范围）**。前置验收为 `e0f440067792dc661e99a4f861a4116eeab4d304`，实现提交为
`8486370d13497137529cdd1efb7602608a27d85b`，分支为 `codex/editor-redesign-v4`。本目录是阶段结束后的冻结快照；
唯一可变施工材料仍为 `.internal/editor-redesign/`。未修改 main，停在 P03 等待复审，不进入 P04。

## 实现与唯一所有权

- `editor/tools/material/model/` 提供一个 STATIC `material_model` 构建边界，安装包为
  `lux-engine-editor-material-model`。公开依赖为纯历史、会话和既有材质图，独立消费者不启动窗口、GPU、预览或编译器。
- `SessionStore` 独占 `MaterialSession`；会话持有唯一 `MaterialSource`、既有 History 和 SessionState。
  没有另设 current、dirty 或 busy。私有 `currentContent()` 直接查询历史身份，不调用完整 describe。
- 普通字段批次仅暂存被访问的值；结构批次按顺序解释，节点替换/删除重建结束旧载荷的生命周期，后续字段针对新节点。
  准备失败不改变源、历史、保存基线或观察版本；成功只提交一次既有历史操作。
- `withRead()`、深捕获、编码及重载准备使用同一准入 gate，覆盖扩展 clone 回调、临时对象析构及异常退出。
  重载采用保留 SessionId、更新 HistoryId；过期候选不能覆盖新状态。
- 冻结快照独立拥有图和外部 CodeLease。历史先于源销毁，节点析构返回后才释放代码；快照移动赋值也遵守此顺序。

## 迁出、删除与暂留

已从旧 `MaterialEditor::Impl` 删除纯算法体及相应声明：`GraphDelta`、`GraphEditOperation`、`TValueEdit`、
`editGraph`、`change` 和各私有值访问算法。旧产品转为构造领域意图并调用同一 `prepareMaterialEdit()`；
没有复制两份历史或图编辑算法，也没有包装旧 Impl 作为新模型实现。

旧产品的 UI、预览、编译、IO、项目资产检查和调用适配继续保留。旧窗口尚未切换到 MaterialSession，仍管理旧源/历史；
并未为旧窗口再创建一份同步的新会话。限定消费者、逐成员去向和 **P12 删除期限**见
`migration-ledger.json` 的 `p03_disposition`、`p03_extractions`、`p03_retained_adapters`。
继承的唯一 `editor/transition/` 私有桥不变；新模型禁止依赖该桥及旧 Context/Editor。
本轮删除的是算法与声明，未宣称整个旧 MaterialEditor 已删除。

完整增改文件与实现 Git blob 校验和在 `files.json`；原 P00、P01、P01-R1、P02、P02-R1 快照保持原样。

## 行为与工程证据

| 验收 | 实际执行和证据 |
| --- | --- |
| X03-01 | 无预览的真实 CPU 材质图：节点、连接、值、纹理/参数槽、协调声明类型更新、Undo/Redo、冻结；`logs/material-content.log`，独立安装消费者 |
| X03-02 | 插入/替换失败、候选销毁计数、准备/历史预算拒绝；比较完整编码、历史 cursor/current/entries/revision/charge、observed 与 dirty；`logs/material-failure.log` |
| X03-03 | 节点位置编码往返及 Undo/Redo；视图 pan/zoom 不在源或快照中；`logs/material-layout.log` |
| X03-04 | 真实虚函数 clone/destructor 的 ConstantNode 特化，外部代码 owner 弱引用断言；快照跨替换和关闭存活、最后节点析构之后才释放代码；`logs/material-lifetime.log` |
| 继承混合批次 | 替换、删除重建前后的字段顺序、完整图 Undo/Redo、非法顺序原子失败与 NO_CHANGE；`logs/material-mixed-*.log` |
| 稳定读取 | clone、临时析构、非法 clone、异常展开中拒绝编辑/undo/redo/close/rebind/reload/嵌套读取，退出后恢复；`logs/material-reading.log` |
| P01/P02/R1 | 原 51 项测试名称及原行为断言保留；原 Scene 13 个独立场景、sessions/关闭补正详细日志和历史验证器重跑 |
| 全量工程 | 显式 `LUX_EDITOR_MIGRATION_STAGE=P03`，`target all -j 4 -- -k 0`；第二轮无工作；完整 CTest **59/59** |
| 安装 | 重装 SDK；七组安装消费者重新配置、构建、二次无工作和 CTest，合计 **8 项通过**，包含新的纯 CPU Material 消费者和原外部插件/GPU消费者 |
| 依赖负例 | 实际 CMake 图的直接、传递、alias、LINK_ONLY、STATIC PRIVATE、imported、编译器、UI、Runtime、Storage、旧桥/Editor 头负例，修复每个图后必须通过；`evidence/*boundaries.json` |
| 干净副本 | tracked snapshot 门禁后，从实现 SHA 独立 clean clone 配置并检查实际完整 target 图；`logs/clone-*.log` |
| 历史来源 | P00/P01/P01-R1/P02/P02-R1 的源码按各自 implementation_sha 物化验证，不用当前源码冒充历史；`logs/historical/` |
| 最终 P03 门禁 | `python dev_log/P03/check_receipt.py`，只使用归档路径及 Git 对象；另验归档换目录和故意缺失必需日志，见 `validation/` |

相关 Q01/Q07/Q09/Q11/Q27 只标记 Material 领域的 `SCOPED_PASS`，具体边界在 `receipt.json`。
没有把 P05 持久化服务、P07 编译/预览服务、P10 视图或 P13 总体验收提前判定完成。
首次开发验证遇到的真实失败及修正保留在 `logs/development/`，没有删除原断言或用测试数量代替语义验收。

## 保持失败的既有缺陷

| ID | 本轮实际结果 | 后续责任 |
| --- | --- | --- |
| C01 | **FAIL**，`visible_before=0 visible_after=1` | P09/P12 |
| C03 | **FAIL**，`released_during_query=1` | P11 |
| C04 | **FAIL**，`create_succeeded=1 outcome_succeeded=0` | P12 |

三项原复现 exit=1，原判定未改、原源码未修。P03 不把它们改成 PASS 或 SKIP。

## 明确保留的限制

- 结构候选临时深复制整张图，历史只保留受影响差异；普通字段编辑保持局部，不调用无关节点 clone。
  预算统计已知载荷与容器存储，不是 allocator 总开销或进程 RSS 上限。
- 内置节点移除 final，允许保持既有 kind/schema 的动态特化；Node 的构造权限仍封闭。
  本轮没有新增节点种类、磁盘格式或任意插件节点编解码/编译框架。生命周期测试使用真实派生节点和代码 owner，
  不冒称加载了一个新增 DLL；原外部插件消费者另行重建运行。
- 源对同一代码 owner 去重，并保守保活到重载/关闭；快照独立保活，不提供每次删节点立即卸载插件。
- 修改的 modules 公共头已同步 Debug、RelWithDebInfo、Android include 三个前缀；Android 仅同步头，没有配置/构建。
- 独立 clean clone 验证配置与依赖闭包，没有再做第二次全量引擎构建。沿用并重跑原自动 GPU/产品回归，
  不声称本 CPU 阶段新增人工桌面或 IME 验证。

`receipt.json` 记录命令、退出码及日志哈希，`artifacts.json` 校验冻结文件，`files.json` 校验实现 Git blob。
生产机器绝对路径仅作为原始命令历史保存，不作为归档验收器读取来源；缺失必需证据时验收失败。
