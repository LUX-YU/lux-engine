# L0：锁定输入、扩大逐文件计划、先识别混合责任

**本批次不改变生产行为。完成后应有可执行计划，不是泛泛架构感想。**  
前置阅读：00_MASTER、01_DECISIONS、02_PATH_TYPE_TARGET_MAP、04_BUILD_AND_DEPENDENCY。

## 1. 建立唯一施工位置

继续使用 `.internal/editor-redesign/`。已有 JSON 账本能够容纳 layering 时直接追加节点；若过大，可在同目录建立一个 `layering/` 分片，但主账本只引用该分片，不复制同一文件/target/测试状态。

最少数据：

| 分片 | 必备内容 |
|---|---|
| baseline | input SHA、实际工作 HEAD、分支、依赖 SHA/安装路径/构建工具、用户改动原字节 |
| decisions | D01–D07 及动工发现的真实差异、处理理由 |
| file_plan | 每个 tracked Editor 文件、原 blob、目标层/路径、动作、调用者、批次、验收点 |
| targets | 每个生产/测试/生成/安装 target 的角色、层、来源、依赖、STATIC/SHARED 理由 |
| symbols | 拆分/合并/删除的类型、函数、成员及消费者 |
| verification | 继承测试集合、按批次增量结果、最终同 SHA 资格 |
| handoff | 当前完成批次、下一个允许入口、真实阻塞、用户改动位置 |

允许合并为一个账本文件；不要求制造七份独立文档。

## 2. 工作区与参考版本核对

按以下顺序操作，命令结果存入账本引用的日志，而不是只复制终端截图：

```text
git status --porcelain=v1 -z
git rev-parse HEAD
git branch --show-current
git remote -v
git log -1 --format=fuller
git diff --binary
git diff --cached --binary
```

核对 HEAD 与参考验收 SHA 的祖先关系。分支有新的合法提交时，差异归入本轮输入，并说明已完成哪些目标；不反复实施或恢复旧文件。

`ProjectBuilder.cpp` 保护流程：
1. 读取实际 bytes/hash 与 tracked blob 分开保存。
2. 导出未提交 binary diff，保存同目录未跟踪关联文件清单。
3. 不执行 `git add .`，不对带用户差异的原工作区直接批量 git mv。
4. 首选独立 detached worktree 或 clone 完成 tracked 迁移和资格，原工作区完全不动。若需推送实施分支，普通 fast-forward push 当前实现 HEAD 到指定远端分支；有并发新提交先处理，绝不 force。
5. 交付记录用户补丁对应的新逻辑路径；未经授权不替用户提交该补丁。报告区别“实施检出干净”与“原工作区仍有用户修改”。
6. 用户修改后的代码若与新头路径不兼容，只提供单独重定位补丁/冲突说明，不能悄悄把它改写成阶段实现。

## 3. 从真实 Git 清单生成计划

可使用本包 `scripts/plan_inventory.py`：它仅运行只读 git 命令并写一个独立 JSON 草案，不移动、不删除、不 stage。示例：

```text
python scripts/plan_inventory.py --repo <仓库路径> --ref HEAD --rules templates/path-rules.json --out <唯一施工目录>/inventory.proposed.json
```

规则是前缀级**计划种子**，不是已人工审核的全仓清单。脚本未识别的文件必须标 REVIEW_REQUIRED；禁止默认丢弃。CMake/README/生成器/混合目录默认需要人工核验。

将草案并入唯一 `file_plan`，逐项补齐：
- 是否为生产、测试、生成输入、安装支持或历史/旧产品文件；
- 当前实际 target（从 CMake File API 与 compile_commands，不靠文件名猜）；
- 原物理路径、逻辑 include、原 blob；
- 目标层、目标路径、目标 target；
- MOVE / SPLIT / MERGE / DELETE / KEEP / TEMPORARY_P11 / TEMPORARY_P12；
- 本轮删除条件与保留理由；
- 真实 include/链接/生成/SDK 消费者；
- 负责 L 批次和验收条目。

**一对多 SPLIT 必须细到符号或行段职责。** 如 ViewInfo.hpp 分出 ViewError.hpp，不能只写“整理 contracts”。

## 4. 盘点必须覆盖的高风险混合点

| 混合点 | 必须分清的事实 |
|---|---|
| editing 根与 history/sessions | 原 LegacyPersistenceState/EditHistoryTarget 与新 E0 不是一套责任 |
| contracts/ViewInfo.hpp | 纯观察值与 UI 关闭错误的拆分，保留唯一身份 |
| project/ProjectBuilder | 实际为纯配置 Builder，不能按上一稿错误描述归异步层 |
| storage | 通用文件发布与项目目录/项目事务；旧 error 声明不是旧框架链接理由 |
| tools/*/model 的 PersistenceAccess | 作者自身状态操作与 E2 保存角色分开 |
| tasks/ui | Monitor 不需要 UI；View 是消费者 |
| scene/projection 的 test | 集成测试链接 viewport 是合法测试组合，不是生产逆向边 |
| metadata/plugins | UI工厂、纯值、运行装载、贡献安装按语义分类；旧登记暂留不能变基础 |
| views/api 与 layout | 纯布局计划不能借活动 Root，不能靠挪 Interface 目录遮掩 |
| editing/sinclude | InteractionDelivery 归工作台；SignalDelivery/TaskResult 按真实消费者逐个审查 |

## 5. 记录实际 build/安装与依赖

读取 P10Q 收据中的实际配置命令，在独立 build tree 导出当前 CMake File API codemodel/target 及编译数据库。记录原：
- CTest name/labels/command/可用环境；
- 生产能力与测试能力的开关，不能混淆；
- 安装包、export target、公共头和生成头；
- 原 14 组 SDK 及额外质量消费者的真实测试映射；
- 显式 GPU 模式和 native 输入命令；
- 需要保留的进程共享状态和 SHARED 边界。

无需为 L0 重新运行全部旧版 50k 长测。已有 baseline 的逐字日志是事实；用 `ctest --show-only=json-v1` 建立名称清单即可，只有基线构建已坏或不可信时才先作必要恢复。

## 6. 计划内纠正与未知边界

D01 已用真实源码核实，可直接采用。其他分类若与当前代码不符，记录具体文件和调用，先改计划再改代码。

不能把没有读取的文件写成“可安全删除”；不能因为本包路径种子缺一个文件而排除它。发现源文件已在新位置则复用，不再复制回预设路径。

## 7. L0 验收与交付

- 每个 tracked Editor 文件恰有一条去向或明确 split；非 Editor 的受影响 CMake/SDK/脚本也在计划中。
- 所有公开定义的唯一拥有路径可确定；没有多个文件计划生成同名逻辑 include。
- 原新/旧 target 角色标记准确；可选环境与结果范围写清。
- 输入设计的 D01 纠正和 Linux/性能范围调整已新增记录，旧收据字节未变。
- 用户补丁已保全，原工作区没有被清理。

提交仅含本轮施工元数据/必要规则骨架；若 `.internal` 原本不提交，则正常保留本地，最终冻结到 dev_log。记录 `L0 complete` 和 L1 的具体入口，不声明任何新行为通过。
