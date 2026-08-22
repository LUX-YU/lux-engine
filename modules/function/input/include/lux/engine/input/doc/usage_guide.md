# Input 使用指南

公共头位于 `lux/engine/input/`，命名空间为 `lux::input`。平台宿主通常使用完整的
`Input` 对象；不需要窗口的预览、测试和回放也可以单独使用 `ActionMapper`。

## 平台宿主

`Input` 是输入领域的所有权入口。它拥有当前快照、一个 `ActionMapper`、Mapper 内唯一的
`InputActionRegistry`，以及非 owning 的 `InputContextStack`。

```cpp
#include <lux/engine/input/Input.hpp>
#include <lux/engine/window/LuxWindow.hpp>

lux::input::Input input;

const auto jump = input.actionRegistry().registerAction({
    0,
    "jump",
    lux::input::EInputValueType::BOOL
});

lux::input::InputContext gameplay("gameplay");
gameplay.actionMap().bindKey(
    jump,
    lux::input::EKey::KEY_SPACE,
    lux::input::InputValue::makeBool(true)
);
input.contexts().push(&gameplay);

while (!window.shouldClose())
{
    lux::window::LuxWindow::pollEvents();
    input.sample(window);       // 只采样，不更新 Action
    input.evaluate(dt);         // 只消费快照，不访问 Window/GLFW

    if (input.mapper().triggered(jump))
    {
        player.jump();
    }
}
```

UI 宿主在布局和捕获判定完成后传入类别路由：

```cpp
input.evaluate(
    dt,
    !ui_wants_keyboard,
    !ui_wants_pointer
);
```

`sample()` 必须在 Window owner thread 上、事件轮询之后调用。它保留同一轮询周期内的
press/release 顺序与两条 edge，并维护跨帧 held、cursor delta 和 sample timing。

## Headless、测试与回放

合成输入不需要 Window：

```cpp
lux::input::InputSnapshot snapshot;
const auto key = static_cast<std::size_t>(
    static_cast<int>(lux::input::EKey::KEY_SPACE)
);
snapshot.keys_held.set(key);
snapshot.keys_just_pressed.set(key);

input.evaluate(std::move(snapshot), dt);
```

`evaluate(InputSnapshot, ...)` 与平台采样后的 `evaluate(...)` 使用同一 Action Mapping 管线。

## 独立 ActionMapper

纯算法 consumer 可以继续直接构造 `ActionMapper`：

```cpp
lux::input::ActionMapper mapper;
lux::input::InputContextStack contexts;

mapper.update(
    snapshot,
    contexts,
    dt,
    accept_keyboard,
    accept_pointer
);
```

这种用法没有平台采样所有权。Game、Player 和 Editor 不应再另外维护一份 Registry 或
Context Stack；它们应通过 `Input::mapper()`、`Input::actionRegistry()` 和
`Input::contexts()` 访问同一个领域对象。

## Context 生命周期

`InputContextStack` 不拥有 Context。游戏或 Editor 必须保证被 push 的 Context 生命周期
覆盖其驻栈时间，并在销毁前 pop。高优先级 Context 后处理；consume 设置阻断低优先级同类
输入。禁用 Context 不参与求值。

## 注入与重绑定

程序化输入和回放可以使用 Mapper 的 `injectTriggered()` / `injectValue()`。绑定仍通过
`InputContext::actionMap()` 创建和移除；动作名称到 ID 的查询必须使用
`Input::actionRegistry()` 返回的唯一 Registry。

## 平台边界

`lux::input` 公共头不包含 GLFW。Window 只积累 `WindowInputEvent` 原始事实；单一 Input
target 在 CMake 配置期选择 GLFW 或 Android 私有 `.cpp`。这里没有 backend interface、
Adapter target、factory 或运行期注册表。
