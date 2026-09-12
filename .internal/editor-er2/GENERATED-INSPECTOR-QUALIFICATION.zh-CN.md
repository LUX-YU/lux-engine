# ER-2：Editor 生成式 Inspector 交审记录

代码候选：`ef8046f62bcfa3195a9f65b5cfbf3c2cd99654e1`，分支 `codex/shadow-ui-er1`。本轮只完成生成式 Inspector 与菜单接线，
未合并 main、未进入 ER-3、未发布或冻结。开发基线为 a483b701；后续只有交审文档可跟随代码候选。

## 实施结果

- 生成器和 CMake wrapper 位于 `engine/editor/ui/scene/`。复用已有 MetaUnit，生成期递归展开类型和注解，
  每个组件生成一个 `.cpp`，直接调用 ImGui；没有运行期反射字段遍历或注解解释。
- 五个正式组件：Transform2D、Transform3D、Parent、Mesh3D、Light3D；生成对象只链接 editor_scene_ui。
  模块内类型特化与第三方 Editor 插件特化均已编译运行。底层组件声明、modules 和 SceneSession 规则未改。
- 支持已测标量、UTF-8 string、枚举、固定 Eigen 类型、嵌套记录、多维数组、vector（含 bool）、deque/list、
  map/unordered_map、set/unordered_set、optional、variant、pair/tuple。注解决定字段名、提示、只读、
  控件、范围和步长；不适配类型需明确特化，不能用空白字段或原始字节冒充编辑器。
- SceneInspector 继续通过原 SceneSession 的 Transform/Light 手势、预览和历史提交。新增的通用容器
  编辑能力由独立测试 Session 验证；未据此宣称材质、FlowForge 或任意组件已完成业务迁移。
- Window 拥有 Edit 菜单；工具栏和其他场景 Pane 的 Ctrl+Z/Ctrl+Y 固定各自 Session。菜单打开时捕获目标。
  Inspector 中没有文档历史按钮。原 Scene 只读生成入口和手写属性编辑辅助头在消费者迁移后移除。

## 实际资格

| 验证 | 结果 | 原始证据（交付包内） |
|---|---|---|
| 独立 clean clone / all / no-op | 通过，源码 SHA256 构建前后对拍 | evidence/qualification/raw |
| 正常 CTest | 139/139 | evidence/qualification/raw/ctest-details.log |
| 专用诊断 CTest | 159/159 | evidence/current/logs/diagnostic-candidate-ctest-details.log |
| 正常 Scene GPU 路径 | 20 个变体通过，另含生命周期/外来 Renderer/字体资格 | evidence/qualification/raw/gpu-*.log |
| 诊断 Scene GPU 路径 | 18 个变体通过，含构造、资源子失败/背压、共享资源、输入、关闭 | evidence/current/logs/diagnostic-candidate-gpu-*.log |
| 安装与迁移 | 5 组消费者 × 2 个新位置，通过 | evidence/qualification/raw/*-editor-*.log |
| 新增生成代码实际归属 | 5 个正式对象只在 Editor Scene UI DLL；检查 89 个项目 DLL | evidence/current/ownership-q3 |
| 正常/诊断隔离 | 10 个 DLL × 2 类产物的导出、导入、PDB 和分配来源通过 | evidence/current/isolation |
| 安装后再生成 | 直接/传递头、宏、生成器修改、缺失 IR 恢复、准确负例和 no-op 通过 | evidence/regeneration |

上述 CTest 数字是注册测试数，不是逻辑断言数。所有构建使用 MSVC RelWithDebInfo、all、-j 4、-k 0；
构建、测试、消费者与计时串行。构建失败不运行旧 EXE。诊断产物没有安装，也没有用于正式计时。

新增类型测试实际注入 UISession 事件：容器增删及 Undo/Redo、排序键编辑直到结束才提交、optional/variant
切换、UTF-8 文字内部值、递归只读、重复键拒绝、真实容器 allocator 调用失败和同一 operation 的业务失败重试。
七类生成期准确负例分别检查错误原因，并核对失败没有覆盖上一套生成文件。
真实场景测试点击 Edit 菜单，使用 Ctrl+Z/Y 和生成的 Transform 控件，验证实际作者值与七张 GPU 回放图像。

## 修复与证据边界

源码检查修正了 map 键/值 ID 冲突，以及容器修改误清空其自身输入暂存；不把检查发现写成审阅方已运行复现。
开发测试发现的工具栏快捷键绑定缺失已修复并重跑；离屏点击是测试几何配置问题，修正后再验收。
安装消费者曾因缺少匹配的 ImGui 编译依赖失败，Editor SDK 现提供原版头和同一共享后端导入库，使用相对安装引用。

候选 eb9c2db7 的普通资格虽然通过，安装再生成测试仍发现注解更新被 Ninja 跳过：常量 IR 投影不变，而
JSON sidecar 改变。候选 ef8046f6 为第二阶段生成规则接入解析器完整 depfile，并开启已支持的跨头反射收集。
修前断言失败、修后直接/传递头与宏更新、恢复后 no-op 都保留原记录。早期 591242ef、eb9c2db7 的有效普通
资格保留各自身份，不能替代本候选结果。

## 有限成本

同一 i7-13700KF，1 个 Pane、1000×800、0 个 Scene View；固定逻辑 dt=1/60，没有帧节奏等待。
每个独立进程 100 帧 warmup，随后 10000 帧；五组 manual/generated 配对，交替运行顺序。
每次实际工作量均为 5,040,000 个顶点、11,670,000 个索引，独立模型 checksum=370000。

| 版本 | 五次 callback 批量 ms | callback 中位 ms | 整帧中位 ms |
|---|---|---|---|
| manual | 34.852, 34.044, 34.056, 35.857, 35.882 | 34.852 | 74.259 |
| generated | 33.846, 35.098, 34.833, 33.455, 34.299 | 34.299 | 73.521 |

callback 与整帧捕获分别计墙钟区间；这些是主动调用路径的用时，仍包含系统调度扰动。
没有逐操作 p95，没有 GPU 等待归因，也不据此宣称所有类型加速或大型容器性能已验证。
原始十次进程结果、CPU、程序 SHA256 和输入说明位于 evidence/current/cost。

## 源码与产物身份、限制

source-and-reports.zip 包含代码候选的实际 Git 源码、必要原始日志、生成代码、结果和 manifest。
Git blob OID 与 Windows clean checkout SHA256 分列；SDK 原位置/迁移位置、正常/诊断 EXE/DLL/LIB/PDB
分别记录身份。源码 blob 和归档证据逐项校验后才交付，不使用摘要代替文件。

用户此前确认的真实 IME、鼠标捕获、中键平移/右键旋转保持原有验收范围。本轮 UiText 注入不等同新的
Windows IME 桌面验收；生成式属性、菜单和关闭确认按钮仍需独立桌面审阅。
复杂类型必须遵守草稿所有权协议；默认结构修改需要可无异常 swap 的值，其他组合需明确特化。
Scene 当前仍是有限内存编辑，没有正式保存 codec；没有扩展材质/FlowForge 业务，也没有引入通用 Context。
