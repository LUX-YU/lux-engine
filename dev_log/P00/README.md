# P00 验收：基线、迁移清单与专项检查

状态：**PASS（仅 P00）**。输入提交 `2bb33ff1a1f11025cf404e074c0e9b259239d8a4`；
实现提交 `c61c11941694ac3499d520e0b4c9e9bfe82bbf46`。本记录单独提交，避免收据引用自身 SHA。
P01 尚未开始。

## 已完成的变化

- 增加五组 Editor 测试开关，native 默认开启，其余显式选择；多种要求取交集。
- 将 lld-link/PowerShell 查找限制在实际需要的集成组，删除测试配置中的固定 D 盘 hint。
  Flow 产品功能保持启用。单独的原生 Flow 图/导出/源码编码验证不链接 UI 或 Flow 编译器。
- 为本轮迁移增加直接/传递依赖、私有头、到期路径检查。CMake 解析 alias；条件链接按保守并集检查，
  `LINK_ONLY` 与显式生成依赖不丢弃。负向夹具先因指定规则失败，修正后在同环境通过。
- 复用私有 EditorTestAccess 建立三项真实缺陷复现。唯一产品代码改动是测试配置下的连接故障注入；
  普通产品构建不包含该注入，不改变旧问题的运行语义。
- 正常构建不读取 `.internal` 或 `dev_log`，没有新增业务 owner、空模块、DLL 或公共产品接口。

## 盘点结果及证明边界

| 重点对象 | 显式成员数（含该 TU 可见的内部辅助结构） |
| --- | ---: |
| Editor::Impl | 85 |
| SceneEditor::Impl | 204 |
| MaterialEditor::Impl | 144 |
| FlowForgeEditor::Impl | 192 |
| EditorContext 及 Impl | 50 |
| PaneManager | 26 |
| EditHistory 及 Impl | 46 |
| 合计 | 747 |

七个翻译单元使用实际编译数据库的定义、包含路径和 C++20 参数，经现有 clang-cl 成功解析。
每项有职责、去向、首次处理阶段及删除期限，未分类项为零。511 个种子条目中，488 项与重点 AST
匹配；其余 23 项是 SceneSaveCapture、历史值、SceneRuntime 和登记/请求类型，已另核对源声明。
61 个待迁移文件均实际存在；本阶段没有提前删除它们。

`source-inventory.json` 固定 818 个范围内源文件；测试覆盖记录包含 44 个文件的 2223 处断言起始位置。
该索引用于追踪迁移，不声称所有分支都已执行。原有 28 个 CTest 名称在完整配置下全部保留。
命名相同的未来 `lux_editor` 与当前 `editor/app` 入口明确区分，旧入口仅暂留到 P12。

AST 引用只证明所解析翻译单元中的解析结果；`candidate_files` 是包含同名 token 的检索范围，可能含
局部变量、注释和同名符号，不冒充解析完成的全仓交叉引用。`declaration_candidates` 同样是文本候选。
公开接口、生成器、安装消费者及兄弟私有头依赖通过文件/target 清单另外核对。
检查器不能证明运行期反向指针、代码寿命、间接动态加载依赖或所有权；这些仍由后续行为验收负责。

## 验收结果

| 项目 | 结果 |
| --- | --- |
| 修改前全量构建及 CTest | 无新增工作；28/28 |
| 实现提交全量 `all -j 4 -- -k 0` | PASS |
| 第二轮构建 | `ninja: no work to do` |
| 完整 CTest | 31/31，16.65 秒；原 28 项 + 2 项架构检查 + 1 项原生 Flow 检查 |
| X00-01 / Q45 | 禁止依赖、跨私有头和路径期限夹具通过 |
| X00-02 / Q49 | 独立 native-only 配置通过；拦截禁止 Editor 平台测试工具查询；Flow/Editor 产品目标保留 |
| X00-03 | 七组 AST 记录与清单逐项相等，处置与期限齐全 |
| X00-04 | 原测试没有减少；三项缺陷维持独立失败结果，不注册 WILL_FAIL |
| 安装消费者 | 四组 5/5：生成组件、外部 Editor/GPU 插件、SceneElement、UI feature |
| 消费者第二轮构建 | 四组均无新增工作 |
| V4 附带静态扫描 | 扫描 1578 个源文件，无已列规则违反；不等于语义验收 |
| `git diff --check` | PASS |

native-only 指 Editor 测试分组，不意味着整个 Editor 产品无需图形 SDK，也不关闭 Engine/modules
既有测试。独立配置没有重新构建整套基础库；原生 Flow 可执行文件已在完整构建和 CTest 中真实运行。
这不是 clean-clone / foundation qualification。

## 仍然失败的旧行为

| 问题 | 实测结果 | 后续责任 |
| --- | --- | --- |
| C01 / Q31 | 坏 docking 被拒绝，但目标窗口 visible 已从 false 变为 true；数量及原 dock 数据未变 | P09 验证/计划，P12 原子采用 |
| C03 / Q38 | QUERY 替换命令登记成功，旧代码保活对象在回调返回前释放 | P11 不可变扩展及调用保活 |
| C04 / Q42 | 注入连接容量错误后，create 返回成功对象，outcome 已失败 | P12 创建结果传播 |

三个复现程序均退出 1，并报告原正确契约失败。没有把它们反向断言为成功，也没有在 P00 顺手修复。
P00 允许记录这些已知旧缺陷；其对应 Q 场景不能标为最终通过。

## 修正过的验证环境/测试问题

- 初次复现未显式提供有效 ExecutionRuntime 配置，属于 setup 错误；补齐配置后重新运行。
- 新增原生 Flow 测试最初将稀疏索引当作连续下标，发生崩溃；改用已有 getNode 并初始化反射后重跑
  完整 CTest。失败日志仍留在外部构建目录，不算产品已知缺陷。
- native-only 查找拦截最初未向调用作用域传回 find_program 结果；修正测试脚本后重新配置。
- 安装消费者首次缺少安装 DLL 搜索路径，停在加载阶段；只结束了本次启动的测试进程。
  最终显式加入安装 bin 和 vcpkg bin，并使用 CTest 120 秒超时，四组全部通过。初次失败记录保留。

## 交接与未执行范围

- 没有改变资产格式、SceneRuntime/RenderResources 契约、插件协议或旧业务类型；没有修改 modules 公共头，
  因而无需三个安装前缀的公共头同步。当前 RelWithDebInfo 安装已更新。
- 未进行 Android、真实中文 IME、完整联合桌面操作、新的性能/内存资格或 clean-clone 验证。
  原 S09 和 V4 后续阶段要求仍未完成；旧批次数据不替代本次未执行项目。
- 原始规范的 39 个逻辑 target 没有变成 39 个空库；已存在同名入口及未来模块状态见 architecture.json。
- 新模块不允许包含旧 Context/Editor 头。旧模块当前存在的私有头借用仍按清单到期清理，不能作为新模块先例。
- `editor_baseline_failures`、菜单注入及相关 TestAccess 随旧框架最迟在 P12 删除；行为断言迁入新 owner 测试。
- 下一阶段仅 P01：历史纯化、保存 checkpoint 和会话所有权。读取 receipt.json、迁移账本以及本地 V4 原件后开始。

完整命令、环境、退出码、日志哈希见 receipt.json/baseline.json；本阶段文件列表见 files.json。
