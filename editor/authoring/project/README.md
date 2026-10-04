# 项目描述与内存构建

`editor_project` 只提供 ProjectManifest、纯 codec/验证、ProjectBuildConfig 和 ProjectBuilder。
它不创建目录、持有文件锁、运行异步任务或依赖 UI。
安装消费者使用 `lux-engine-editor-project` / `lux::engine::editor::editor_project`。

ProjectBuilder 只生成创建配置：项目身份、名称、插件选择以及可选初始 ScenePackage。
最小项目仅需 Project.luxproject；Content 由用户按内容包组织，Beginner 是可选初始包。
源相对路径与 /Project 下的资产 VFS 路径分别保存，不互相推导。

磁盘读写由 editor/activities/project 的 createProject / ProjectStorage 完成。
ModelImporter 位于 activities/project，由 EditorApplication 装配并借用同一 ProjectStorage。
ModelImporter 使用 engine/toolchain 编译器；其它引擎消费者可以直接调用这些编译器，不需要 Editor。

SessionStore 唯一拥有三类作者会话及其 History；具体视图只借用会话与交互。
内容打开、保存和发布归 activities，应用负责组合与关闭审阅。
窗口隐藏不会停止已接纳的导入或发布工作。

`engine/project/plugins` 是运行期插件装载与元信息能力，供游戏和编辑器共享；
本目录是编辑器项目描述。Editor 专用配置和组件 UI 插件位于 `editor/workbench/desktop/modules`，不会进入游戏链接依赖。

保留的快速测试验证保存目标冲突与发布行为。资产切换、首次保存、另存为、
未知数据保留和关闭流程由编辑器工作流用例覆盖。
