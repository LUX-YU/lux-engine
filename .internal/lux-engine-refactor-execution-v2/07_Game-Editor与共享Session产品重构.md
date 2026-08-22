# Game、Editor 与共享 Session 产品重构

> 删除 Host 产品层与公开 Runtime 容器，让导出游戏无 Engine/Editor 语义，并使 Editor Play 复用同一游戏执行会话

**执行文档 07 · 重构实施版 v2.0**

| 项目 | 内容 |
| --- | --- |
| 代码基线 | `LUX-YU/lux-engine@09b2a82582550bcbe03afeef77d2591e1656a656` |
| 基线日期 | 2026-08-17 |
| 文档日期 | 2026-08-18 |
| 适用对象 | Game、Player、Editor、Platform Entry、Game Exporter 与产品测试负责人 |
| 文档地位 | 架构决议与施工合同；实施分支前移时按符号和职责重新定位，不得机械照搬行号 |

> 本版以“`modules/` 是可独立分发的公共 SDK 边界”为首要前提。任何为消除依赖环而把 Engine、Scene、Editor 或 Extension 协议下沉到 `modules/` 的做法，均视为架构回归。

> **2026-08-20 裁决更新：** Game/Editor 继续装配现有公共 `AssetManager` 与 `engine/runtime/assets`，并在标准 Codec descriptors 后追加 Engine Scene descriptor。本文中的 `AssetStore` 与 `ScenePackage` 未来接口示例已被取代，现行类型为 `AssetManager` 与 `SceneAsset/SceneDescription`。启动清单必须显式指定 boot Scene。


## 1. 核心关系

正确关系不是：

```text
EngineRuntime
├── GameRuntime
└── EditorRuntime
```

而是：

```text
公共 Modules + ECS + Engine 领域实现
                  ↓
        game::Session（共享执行实例）
          ↙                 ↘
game::Game 产品          editor::Editor 产品
```

Editor 包含“创建游戏会话的能力”，而不是继承或嵌入完整 Game 产品。

## 2. 目标目录

```text
engine/
├── game/
│   ├── include/lux/game/
│   │   ├── Game.hpp
│   │   ├── GameConfig.hpp
│   │   └── Session.hpp       # 可设为 private install 或不安装
│   └── src/
├── editor/
│   └── ...
└── game/deployment/
    ├── GameManifest.hpp
    └── GameManifestCodec.cpp

products/
├── player/
│   ├── desktop/main.cpp
│   └── android/...
├── editor/
│   └── main.cpp
└── launcher/
```

最终删除 `engine/hosts` 作为通用层。平台入口是产品源文件，不需要 Host 类。

## 3. `GameApplication → Game`

### 3.1 文件迁移

MOVE：

```text
engine/hosts/game_application/include/.../GameApplication.hpp
→ engine/game/include/lux/game/Game.hpp

engine/hosts/game_application/src/GameApplication.cpp
→ engine/game/src/Game.cpp

engine/hosts/game_application/src/GameApplication.State.inl
→ engine/game/src/GameState.hpp 或 Game.cpp 内部 State
```

DELETE：

```text
engine/hosts/game_application/CMakeLists.txt
lux::engine::host::game_application
```

### 3.2 目标 API

```cpp
namespace lux::game
{
    class Game final
    {
    public:
        static expected<Game, OpenError>
        open(GameConfig, platform::NativeWindowHandle);

        Game(Game&&) noexcept;
        Game& operator=(Game&&) noexcept;
        ~Game();

        expected<void, FrameError>
        frame(const input::InputSnapshot&, math::Extent2u);

        CloseTask close();

    private:
        struct State;
        std::unique_ptr<State> state_;
    };
}
```

公开 Game API 不返回：

```text
Executor&
AssetStore&
Renderer&
ExtensionLoader&
Scene&
```

调试与测试如需观察，使用窄 `GameDiagnostics` Snapshot，而不是暴露内部 owner。

### 3.3 `Game::State`

```cpp
struct Game::State
{
    events::Events events;
    engine::execution::Executor executor;
    asset::Vfs vfs;
    asset::CodecRegistry codecs;
    engine::assets::AssetStore assets;
    render::Renderer renderer;
    engine::extensions::ExtensionLoader extensions;
    input::Input input;

    std::unique_ptr<game::Session> session;
    GameFrames frames;
    GameState state;
};
```

这是 `.cpp` 内私有存储，不是新的架构对象，不提供 `get<T>()`。

## 4. `game::Session`

### 4.1 职责

一个 Session 表达一次游戏执行：

```text
解析 Scene Package
验证 Required Extensions
创建 Asset View
启用标准 Scene Features
创建主 Scene
安装 Script Feature
加载启动 Sections
管理暂停、时间尺度和会话级输入
关闭 Scene 与会话级任务
```

不负责：

```text
Window
Renderer 设备所有权
Executor 线程池所有权
AssetStore 进程缓存所有权
Editor Workspace
```

### 4.2 API

```cpp
namespace lux::game
{
    struct SessionDependencies final
    {
        engine::execution::Executor& executor;
        engine::assets::AssetStore& assets;
        render::Renderer& renderer;
        engine::extensions::ExtensionLoader& extensions;
        events::Events& events;
        input::Input& input;
        engine::scene::FeatureCatalog& sceneFeatures;
    };

    class Session final
    {
    public:
        static expected<Session, OpenError>
        open(
            const engine::scene::package::ScenePackage&,
            SessionDependencies);

        void update(const SessionFrame&);
        engine::scene::Scene& scene() noexcept;
        CloseTask close();

    private:
        struct State;
        std::unique_ptr<State> state_;
    };
}
```

Session 接收精确依赖引用，不接收 `Game&`、`Editor&` 或 Engine Runtime。

## 5. Editor Play 复用 Session

### 5.1 当前正确机制

当前 `EditorScene::enterPlay()` 已经先通过生产 Cook 路径生成独立数据，再启动独立 Scene；这一行为必须保留。

### 5.2 目标流程

```text
SceneDocument
→ save/validate authoring state
→ SceneCooker produces immutable ScenePackage + Section images
→ create play Asset View
→ game::Session::open(...)
→ Editor frame drives Session::update(...)
→ exitPlay closes Session
```

禁止：

```text
直接模拟 Edit registry
复制 GameApplication::Impl 装配
为 Editor 建立另一套 Scene Feature
跳过 production codec
```

### 5.3 Editor State

```cpp
struct Editor::State
{
    events::Events events;
    engine::execution::Executor executor;
    asset::Vfs vfs;
    asset::CodecRegistry codecs;
    engine::assets::AssetStore assets;
    render::Renderer renderer;
    engine::extensions::ExtensionLoader extensions;
    input::Input input;

    Workspace workspace;
    Workbench workbench;

    std::unique_ptr<game::Session> play;
    std::vector<std::unique_ptr<PreviewScene>> previews;

    EditorFrames frames;
};
```

注意：Editor 与 Game 拥有同“种类”的基础能力，但不要求共享同一个顶层对象或继承关系。

## 6. 删除 `GameHost`

### 6.1 当前文件

```text
engine/hosts/player/include/.../GameHost.hpp
engine/hosts/player/src/GameHost.cpp
engine/hosts/player/app/main.cpp
```

### 6.2 目标

`products/player/desktop/main.cpp` 直接装配 Window 与 Game：

```cpp
int main(int argc, char** argv)
{
    auto manifest = game::deployment::GameManifest::load(...);
    if (!manifest)
        return report(manifest.error());

    auto window = platform::glfw::Window::open(
        {.title = manifest->title});
    if (!window)
        return report(window.error());

    auto game = game::Game::open(
        makeGameConfig(*manifest),
        window->nativeHandle());
    if (!game)
        return report(game.error());

    while (!window->closeRequested())
    {
        window->pollEvents();
        auto input = platform::glfw::captureInput(*window);
        if (auto frame = game->frame(input, window->extent()); !frame)
            return report(frame.error());
    }

    return finish(game->close());
}
```

平台入口可有小型局部 helper，但不再创建 `GameHost` 类。

## 7. Editor 产品入口

MOVE：

```text
engine/hosts/editor
→ products/editor
```

产品入口只负责：

```text
命令行解析
Window 创建
Editor::open
平台事件循环
顶层错误报告
```

Editor 实现位于 `engine/editor`，产品入口不拥有业务 Controller。

## 8. Game Manifest 与导出语义

### 8.1 文件迁移

MOVE：

```text
modules/resource/deployment/RuntimeLaunchManifest.*
→ engine/game/deployment/GameManifest.*
```

### 8.2 字段

RENAME：

```text
game_pak    → gamePack
engine_pak  → basePack
boot_scene  → bootPackage
extensions  → requiredExtensions
capacity    → productOptions/renderOptions（按领域拆分）
```

目标：

```cpp
struct GameManifest final
{
    static constexpr std::uint32_t schemaVersion = 5;

    std::string title;
    std::filesystem::path basePack;
    std::filesystem::path gamePack;
    std::string bootPackage;
    GameOptions game;
    render::RenderConfig render;
    std::vector<RequiredExtension> requiredExtensions;
};
```

### 8.3 导出目录

导出程序中避免 Engine/Editor 语义：

```text
MyGame/
├── MyGame.exe
├── game.toml
├── content/
│   ├── base.pak
│   └── game.pak
├── extensions/
└── libraries/
```

不输出：

```text
engine.pak
engine_runtime.dll
editor_*
EngineRuntime
```

内部动态库文件名若暂时含 `lux_engine_*`，可作为后续 ABI/发布迁移；用户可见 Manifest 与 API 先清理。

## 9. Game 与 Editor 的基础能力复用方式

### 9.1 共享 Builder 函数，而非共享 Runtime 容器

建立私有创建函数：

```cpp
expected<execution::Executor, Error>
openExecutor(const ExecutionConfig&);

expected<assets::AssetStore, Error>
openAssets(asset::Vfs&, asset::CodecRegistry&, execution::Executor&);

expected<render::Renderer, Error>
openRenderer(const render::RenderConfig&, platform::NativeWindowHandle);

expected<extensions::ExtensionLoader, Error>
openExtensions(...);
```

Game 与 Editor 调用同一函数，但各自显式拥有结果。

### 9.2 领域 Bundle 仅允许私有返回值

若构造错误回滚需要一次返回多个对象，可使用 `.cpp` 私有：

```cpp
struct BaseFacilities
{
    Events events;
    Executor executor;
    Vfs vfs;
    CodecRegistry codecs;
    AssetStore assets;
    Renderer renderer;
    ExtensionLoader extensions;
};
```

禁止安装头文件、禁止 `get<T>()`、禁止业务对象保存 `BaseFacilities&`。

## 10. Frame 循环

### 10.1 Game

```text
poll input（产品 main）
→ Game::frame
  → drain executor main-thread completion
  → dispatch events
  → update session/scene
  → render frame
  → commit safe-point mutations
```

### 10.2 Editor

```text
poll input（产品 main）
→ Editor::frame
  → drain executor
  → update filewatch/content
  → update active documents
  → update optional play session
  → update previews
  → draw workbench
  → render
  → commit safe points
```

共享顺序可由私有 `FramePhases` helper 表达，不能再次建立公共 `FrameCoordinator` 服务。

## 11. 关闭协议

### 11.1 Game

```text
close product admission
→ close Session
→ close Extensions
→ close AssetStore
→ close Renderer
→ close Executor
→ release Window in product main
```

### 11.2 Editor

```text
stop import/compile/filewatch producers
→ close Workbench connections
→ close play Session
→ close PreviewScenes
→ close Documents/Workspace
→ close Extensions
→ close AssetStore
→ close Renderer
→ close Executor
```

析构只做同步兜底或断言已关闭；复杂异步关闭必须由 `close()` 完成。

## 12. CMake 迁移

### 12.1 新目标

```text
lux::game
lux::editor
lux_player
lux_editor
```

`lux::game` 是 Engine 产品库，可供 Editor 内部链接；它不等于 `lux_player` 可执行程序。

### 12.2 删除

```text
lux::engine::host::game_application
lux::engine::host::player
engine/hosts/game_application
engine/hosts/player
```

### 12.3 依赖

`lux::game` 不依赖：

```text
GLFW
ImGui
Editor
Authoring
Toolchain importer
```

`lux_player` 依赖：

```text
lux::game
lux::window_glfw
```

`lux::editor` 可以依赖 `lux::game` 的 Session 实现，但不依赖 `lux_player`。

## 13. Pull Request 序列

| PR | 内容 | 退出闸门 |
| --- | --- | --- |
| PRODUCT-01 | 新 `Game` façade 包住 GameApplication | 行为不变 |
| SESSION-01 | 提取 `game::Session` | Player 仍通过旧 façade |
| SESSION-02 | Editor Play 改用 Session | Play/Cook 一致性 |
| PRODUCT-02 | Product main 直接驱动 Game | GameHost 无消费者 |
| PRODUCT-03 | 删除 GameHost 与 game_application target | Desktop/Android 测试 |
| MANIFEST-01 | GameManifest 新路径和字段外观 | 旧 manifest 可读 |
| EXPORT-01 | 导出目录去 Engine/Editor 用户语义 | inventory 测试 |
| PRODUCT-04 | `LuxEditor → Editor` façade | Editor 行为不变 |
| PRODUCT-FINAL | 删除 engine/hosts | 所有产品入口位于 products |

## 14. 验收闸门

- [ ] 没有公开 `EngineRuntime`、`GameRuntime`、`EditorRuntime`。
- [ ] `Game` 与 `Editor` 是独立 owner。
- [ ] Editor Play 与 Player 使用同一个 `game::Session::open`。
- [ ] `GameHost` 已删除。
- [ ] `GameApplication` 已删除或仅作为短期 deprecated façade。
- [ ] `lux::game` 不依赖 GLFW/ImGui/Editor。
- [ ] `lux::editor` 不依赖 Player executable。
- [ ] 导出 Manifest 不包含 `engine_pak`。
- [ ] 导出目录无 Editor/Toolchain 二进制。
- [ ] Product API 不公开内部 Executor/AssetStore/Renderer。
- [ ] Close 失败可诊断，不以 `Impl::release()` 泄漏整个产品状态作为常规路径。
