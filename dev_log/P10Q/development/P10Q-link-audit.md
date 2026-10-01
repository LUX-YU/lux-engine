# P10Q 实际链接边界

实现：`3c20910d05d635078ef9021a3a3318086a792b8d`。统计来自 CMake File API，而非目录数量。

统计范围：CMake source 位于 editor/ 的全部 target，包含明示的测试程序及测试 domain/GUI DLL。它不是产品启动装载数。

| 实际构建类型 | 基线 | 最终 |
|---|---|---|
| STATIC_LIBRARY | 33 | 39 |
| SHARED_LIBRARY | 15 | 10 |
| EXECUTABLE | 44 | 48 |

| Target | 原 TYPE | 现 TYPE |
|---|---|---|
| editor_app | SHARED_LIBRARY | STATIC_LIBRARY |
| editor_canvas_churn_test | — | EXECUTABLE |
| editor_flowforge | SHARED_LIBRARY | STATIC_LIBRARY |
| editor_hierarchy_benchmark | — | EXECUTABLE |
| editor_material | SHARED_LIBRARY | STATIC_LIBRARY |
| editor_persistence_execution | — | STATIC_LIBRARY |
| editor_project | SHARED_LIBRARY | STATIC_LIBRARY |
| editor_quality_test | — | EXECUTABLE |
| editor_scene | SHARED_LIBRARY | STATIC_LIBRARY |
| editor_task_monitor_test | — | EXECUTABLE |
| editor_viewport | — | STATIC_LIBRARY |
| project_io | STATIC_LIBRARY | — |

## 保留的 Editor 动态边界

| Target | 消费者／共享状态理由 |
|---|---|
| consumer_domain | 测试专用领域 DLL，验证安装后边界与析构 |
| consumer_gui | 测试专用 GUI DLL，验证生成控件与跨 DLL 生命周期 |
| edit_history | 全局 History 身份；exe、工具、动态 SDK GUI 共同使用 |
| edit_sessions | Session 身份及唯一 Store 访问边界；多个动态消费者 |
| editor_editing | 窄编辑协议仍供动态 GUI／旧工具消费；不复制其身份职责 |
| editor_metadata | 共享反射／组件／命令登记；插件与产品消费者 |
| editor_ui | 仍有独立 SDK／GUI DLL 消费者；P12 旧壳期限保留 |
| physics2d_editor | 真实动态插件，不能改成产品静态自注册 |
| render_feature_meta | Feature 元信息动态共享状态；编辑器与插件 |
| scene_render_meta | Scene Render 元信息共享登记；编辑器与插件 |

产品内部实际减少五个 SHARED 边界；新增 STATIC target 不计作 DLL 减少。
实际链接命令、dumpbin imports、cdb lmf 装载列表分别保留；调试器启动时装载列表不能替代业务期间动态插件验证。
共享身份／反射状态未放入多个 DSO 的 archive；无 whole-archive、全 exe 符号导出。
SDK 使用全新 prefix；外部依赖种子有固定清单，未复制 Engine 生成头或旧库。
Windows CRT/PIC/导出在本轮配置与实际消费者核验；Linux PIC 实机验证仍 NOT_RUN。
