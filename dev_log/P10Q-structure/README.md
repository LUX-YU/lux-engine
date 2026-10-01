# P10Q-structure 验收

状态：**PASS（本轮明确的 Windows 范围）**。停在 P10Q，等待复审；未进入 P11。

- 输入：`f7c27f9375cbf8dd8af37b30a6027a460de26213`。
- 完整实现：`e72e9f7f931c5825dcd6c2c5a5b40dd66ccf62a5`；验收提交在其后单独形成。
- 门禁：`LUX_EDITOR_MIGRATION_STAGE=P10Q`、`LUX_EDITOR_LAYERING_MODE=STRICT`。
- 原工作区仍在输入提交，`ProjectBuilder.cpp` 用户字节未变且未纳入提交；main 未变。

## 实际交付

五层是职责目录，保留真实的细粒度构建边界，没有增加五个聚合库。

| 层 | 最终职责与唯一实现 |
| --- | --- |
| editing | 原 History、SessionStore、SessionState、身份与共同值；保留共享身份 DLL |
| authoring | Scene、Material、Flow 作者模型、纯 ProjectBuilder、Layout/Recovery 值与计划；不引入 Process |
| activities | 保存与文件发布、Run/Registry 编辑、投影、编译预览、项目/Workspace IO、TaskMonitor |
| workbench | Desktop/ViewHost/API、viewport、widgets、CPU interaction、三工具视图、项目和任务视图 |
| application | `launchEditor()` 的唯一进程启动实现；现有 Launcher/App 直接消费 |

纯分区解码收回 World storage：ScenePackage 直接解码已拥有的内存卷，Process 异步范围读取复用同一个解码核。删除 ScenePackage 的 Process 运输接线及传递依赖。多卷、多 extent、摘要、限制、取消、未知载荷与异步结果对照已运行。

运行实例的 SceneEditing 仍有真实 RunStore 消费者，迁到 activities，未复制历史或另造桥。TaskMonitor 拆为不含 UI 的 `editor_tasks`。`ViewInfo` 留在纯 contracts，关闭错误归 view_api。三个模型的 PersistenceAccess 留在领域内部。生成器、support、安装 provider 和调用方一起迁移，逻辑 include、namespace、原包名与库名保持。

共享 `deliverInput()` 使用左值可调用、同一 `expected<void,E>` 的 concept；Material/Flow 生产调用与四类编译负例均运行。异构会话、保存源、encode job、重绑定与工厂仍保留必要动态边界。没有新 Manager、Runtime、事件总线或服务定位器。

新增的两个实际 STATIC provider 是 `editor_tasks` 和 `editor_launch`。History/Session/Metadata 的共享身份及状态边界未改为重复静态副本；原 tasks_ui、editor_launcher 仍有实际实现，未留下空转发 target。

## 最终验证

所有最终命令绑定上述完整实现 SHA，使用独立 clean tracked clone、新依赖种子和新 SDK 前缀。

| 验证 | 结果 |
| --- | --- |
| Editor 全量 all -j4 -- -k0、二次无工作、完整 CTest | PASS，209/209 |
| CPU native 独立配置、全量构建及 CTest | PASS，183/183 |
| PLAYER 无 Editor 编译单元、全量构建及 CTest | PASS，12/12 |
| 原 14 组 SDK + quality + 4 个最小层消费者 + 2 个显式 GPU 消费者 | PASS，21 组，全部使用新安装 SDK |
| 新 SceneView 双视口 GPU/验证层、Windows 原生输入 | PASS；两个旧 GPU 模式另行执行 |
| N01–N14 实际依赖正例、指定规则拒绝、去边恢复 | PASS，含传递、LINK_ONLY、生成头、模板和未知 imported target |
| 4 项交付 concept 负例、8 项 operation 特殊成员负例 | PASS，生产公共头与实际实例化 |
| clang-cl C++20 消费者、逐个改动公共头独立解析 | PASS |
| 删除指定生成输出后重建及第二轮无工作 | PASS；重建字节 hash 一致 |

原 204 个行为名称全部保留；63 份原 Editor C++ 测试/支持文件去除 include 与空白后的 token 一致。其余路径与规则夹具的等价调整见行为映射和实际日志。测试总数仅作索引，XL01–XL24 的具体行为、测试名及证据见 [receipt.json](receipt.json)。

首次失败没有隐藏：外部依赖 seed 的文本路径差异在核对全部外部文件后定界；早期冷构建为补齐丢失的 README 契约主动中止；下一次完整运行暴露一处旧生成器测试路径，保留原四个断言、修正路径后在最终 SHA 全量重跑。开发期失败/中止日志在 `development/`，不计最终通过。

## 文件、依赖与暂留项

- [file-plan.json](file-plan.json)：518 个原 Editor 文件的原 blob、落点、实际 provider、消费者和最终动作。
- [files.json](files.json)：全部实际增删改文件及固定 Git 对象 hash，含引擎/CMake/消费者。
- [target-map.json](target-map.json)：实际 target 图、文件/生成头 provider 与层分类。
- [abstraction-map.json](abstraction-map.json)：静态/动态边界与未采用抽象的理由。
- [retained-product.json](retained-product.json)：旧产品每个暂留 target 的消费者和 P11/P12 期限。新正式路径不依赖这些旧岛。
- [behavior-map.json](behavior-map.json)、[assertions-preserved.json](assertions-preserved.json)：原行为与断言保留证据。
- [evidence/audit/protected-final.json](evidence/audit/protected-final.json)：原工作区、main、用户字节和历史快照核对。

`ProjectBuilder.cpp` 只迁移 tracked 原体；用户补丁的新位置是 `editor/authoring/project/src/ProjectBuilder.cpp`。已保存原字节和 [重定位补丁](protected/ProjectBuilder-relocated.patch)，对新实现执行 `git apply --check` 通过，未替用户应用。

唯一可变施工记录仍在原工作区 `.internal/editor-redesign/layering/`。本目录是冻结快照；`construction/` 中的账本和脚本不作为第二份可写账本。

## 明确未通过或未执行的既有范围

Linux、系统 IME、ASan：NOT_RUN；本轮未执行 Android 构建。未改 modules 公共头，无三前缀同步事项。

原 P10Q 与旧 50k 深链性能项仍为 PARTIAL；按用户要求未补慢样本。本轮没有改写旧结论。

C01/C03/C04 以原断言再次得到 FAIL：C01 完整 Workspace 产品应用归 P12；C03 动态贡献/代码寿命归 P11；C04 产品关闭结清归 P12。未将本轮新问题挂到这些旧编号。

## 归档验证

使用 `editor/tests/architecture/validate_layering_evidence.py --source <含实现 Git 对象的仓库> --archive <本目录>`。取证使用归档相对路径和固定实现 Git 对象，不需要日志中的生产机器绝对路径。中文/空格路径迁移、实际日志缺失/篡改拒绝及恢复的执行记录见 `archive-probes/`。
