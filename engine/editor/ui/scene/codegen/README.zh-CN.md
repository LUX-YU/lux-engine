# Editor 生成式 Inspector

组件数据和 `LUX_COMPONENT` / `LUX_MEMBER` 注解仍位于原语义包。Editor 的 CMake job 读取已有
MetaUnit JSON，再由本目录的 Python 工具展开类型并生成直接 `ImGui::*` 调用；一个组件对应一个 `.cpp`。
生成物只加入调用方的 EDITOR target。运行期没有反射字段遍历、注解解析或 widget 类型选择表。

```cmake
engine_target_add_imgui_inspector_codegen(NAME project_inspectors TARGET project_editor
    HEADER ${CMAKE_CURRENT_SOURCE_DIR}/Component.hpp LOGICAL_PATH Component.hpp
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/Editor.cpp COMPONENTS project::Component
    CUSTOM_TYPES project::ComplexValue CUSTOM_HEADERS ProjectInspectorWidgets.hpp
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/ProjectInspectorWidgets.hpp)
```

项目内生产 target 使用 `lux_classify_target(... LAYER EDITOR ...)`。独立 SDK 消费者声明其 Editor 所有权
`LUX_ARCH_LAYER=EDITOR`，并私有链接 editor_scene_ui。生成工具不建立 runtime 链接，不向组件的 DLL 注入对象。
安装消费者使用 Editor 包中的原版 ImGui 头和与 SDK 相同的共享后端导入库；不建立第二个 UI context。

## 注解与类型

`display_name`、`tooltip`、`readonly=true`、`widget`、`min/max`、`speed`、`step` 在生成期解释。
`widget` 可选 default、input、drag、slider、color、enum、asset、readonly、custom；不适配的组合、未知类型、
无范围 slider、非有限参数、反向范围、非正 step 都使生成失败。隐藏成员使用 `LUX_NO_MEMBER()`。
数值输入超出注解范围或产生非有限值时保留该字段原值并返回输入错误；业务规则仍由 Session 校验。
控件 ID 使用字段完整身份，不使用显示名称作为唯一身份。
固定向量的各分量在同一行按可用宽度等分；固定矩阵逐行显示。四元数显示为一行 X/Y/Z 欧拉角，
悬停提示单位 Degrees。每个分量保留独立控件身份、范围校验和手势结果。

支持 bool、常见有符号/无符号整数、float/double、UTF-8 std::string、已解析枚举、固定 Eigen 向量/矩阵、
四元数、嵌套反射记录、C 多维数组、std::array、vector（含 bool）、deque、list、map/unordered_map、
set/unordered_set、optional、variant、pair/tuple。结构修改要求对应值类型可无异常 swap；不满足时提供拥有正确暂存协议的特化。
不按原始字节展示未知类型或指针。动态 Eigen 和其他复杂
类型通过 Editor 特化接入。编译通过不代表每个类型已完成真实业务迁移。

## 复杂类型特化

私有 `ProjectInspectorWidgets.hpp` 包含 `<InspectorWidget.hpp>`，定义
`lux::editor::ui::InspectorWidget<project::ComplexValue, DefaultWidgetTag>` 的完整特化，提供：

```cpp
static lux::ui::EditResult draw(project::ComplexValue &draft,
    InspectorInteraction &interaction, const InspectorField &field);
```

它接收拥有的暂存值和 Pane 局部输入状态，调用 ImGui 后返回 changed/began/committed/cancelled。
可通过 `widget=custom, widget_tag=project::SomeTag` 选择编译期特化。特化不得保存暂存值引用、修改 Registry、
操作历史或建立 ImGui context。不要在底层组件头里 include ImGui/InspectorWidget。

## 写入与失败边界

`draw_<组件身份>` 只修改调用方传入的草稿，并不代表作者内容已提交。调用前清理本帧 error；返回后若
`InspectorInteraction.error` 非空，丢弃本次草稿，不向 Session 发送部分结果。选择、owner 或文档切换时
由 Pane 重置交互暂存；增删容器不销毁正在使用的插入键暂存。容器结构修改先复制并准备完整结果，再以
无异常 swap 更新草稿。内存分配耗尽是致命失败，生成入口不捕获或恢复 `std::bad_alloc`；
非法值、重复键等业务错误仍通过有限错误文字报告并保留已提交内容。

SceneInspector 继续调用 SceneSession 的 Transform/Light begin / preview / commit / cancel。
Session 拥有业务内容与 EditHistory；当前通用容器的编辑资格由独立测试业务提供，未新增材质/FlowForge
或任意 Registry 写入。Undo/Redo 位于 Window 的 Edit 菜单及 Ctrl+Z/Ctrl+Y 路由；本地输入固定原 Session。
