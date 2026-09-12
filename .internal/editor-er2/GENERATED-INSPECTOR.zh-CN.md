# ER-2 生成式 Inspector 实施记录

基线 a483b701fb347b4530ba621d680239c92c675400；工作分支 codex/shadow-ui-er1。
本波次实施用户确认的 Editor 层逐组件直接 ImGui 生成方案。未修改 modules、Simulation 组件数据定义、
SceneSession 业务规则或并行 main/Script 工作区。生成器和私有后端构建支持属于 Editor；一般类型支持
与真实业务迁移分别验收，业务写入仍限既定 Transform/Light。

## 变更与验证映射

| 项目 | 实施/实际验证入口 |
|---|---|
| 逐组件编译期展开 | engine_target_add_imgui_inspector_codegen + inspector_codegen.py；真实 MetaUnit；每组件一 cpp |
| 私有后端/SDK | Editor job 私有链接同一共享 ImGui；安装工具自带匹配头与 import library；独立消费者与迁移前缀 |
| 通用类型/自定义类型 | InspectorFixture、CustomInspectorWidget、安装插件特化；C++ 编译与实际 UISession 绘制 |
| 容器与失败 | generated_inspector_types：8 类容器 Add/Remove 与完整模型 Undo/Redo、重复键、真实 allocator 调用拒绝和重试 |
| 历史所有权 | 测试 Session 准备完整替换对象，apply 只 swap unique_ptr；原 Scene 编辑/局部路由测试继续运行 |
| 输入 | UiText 包括 UTF-8 中文内部值、文字占用、只读；真实场景控件拖拽、Edit 菜单与快捷键、取消/重开 |
| 生成失败 | generated_inspector_codegen：非法注解逐类准确拒绝，不覆盖上一套输出；不执行旧 EXE |
| 安装再生成 | VerifyEditorSceneReaderRegeneration.py：注解、宏、生成器依赖、错误保留、恢复与 no-op |
| 成本 | 独立进程 manual/generated 配对，100 warmup + 10000 相同帧，分别记录 callback 和整帧 CPU 工作 |

开发失败原始日志保留。已发现并修正 map 键/值控件 ID 冲突、容器准备误清空其自身输入暂存、安装 SDK
缺少 ImGui 编译依赖；测试驱动显式固定 Pane 几何后再进行坐标点击，不把离屏点击算产品负例。
最终结果和 source/产物身份由新候选的统一资格报告给出，不复用旧日志替代本轮运行。

真实桌面 IME 和鼠标捕获保留用户此前确认的范围；本波次 UiText 注入不是新的 native IME 证据。
生成式控件与新菜单仍需独立桌面审阅。无正式 Scene 保存 codec，不进入 ER-3，不合并 main。
