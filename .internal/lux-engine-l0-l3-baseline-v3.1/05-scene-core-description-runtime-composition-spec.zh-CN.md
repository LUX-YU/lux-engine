# SceneDescription 与 Minimal Scene Core 实施规范（v3.1）

> 状态：Normative。
>
> 施工 Phase：**7**。
>
> 前置：World Phase 2–3、ECS Phase 4、Simulation Phase 5 已通过。
>
> 影响：新增 `engine/scene/core`；SceneDescription codec 可在独立 `engine/scene/asset` leaf。

---

## 1. Scene definition

```text
Scene
    = one loaded WorldDescription
      + one authoritative EnTT Registry
      + one runtime Simulation
      + one Scene cancellation source
```

到此为止。

Scene core 不是：

```text
streaming manager
render scene
process scope
asset residency table
editor document
host/session
```

---

## 2. Public Type Budget

本 Phase 只允许新增：

```text
SceneDescription
Scene
SceneBuildFailure
```

禁止新增：

```text
SceneBuilder
SceneFactory
SceneContext
SceneServices
SceneDependencies
SceneRuntime
SceneManager
SceneRegistry
SceneExecutionContext
```

如果 `Scene::create()` 实现需要 private staged state，使用 `Scene::Impl` / local variables，不创建 public construction framework。

---

## 3. SceneDescription exact durable schema

冻结：

```cpp
struct SceneDescription final
{
    asset::AssetId world;
    asset::AssetId simulation;
};
```

Validation：

```text
world AssetId valid
simulation AssetId valid
```

不得添加：

```text
scene type
streaming index type
load radius
editor state
camera
render settings
window settings
physics backend
product flags
```

Product binary本身知道它链接/注册了哪些 concrete functionality。

当前版本不设计 universal Player executable。

---

## 4. SceneDescription Asset identity

canonical Asset type string立即冻结：

```text
lux.scene.description
```

禁止出现并行名字：

```text
lux.scene
lux.scene.root
lux.scene.runtime
```

旧 legacy SceneDescription 如果含 Entity records，与本类型语义不同；不做 compatibility alias/shim。

---

## 5. Exact Scene public surface

冻结：

```cpp
class Scene final
{
public:
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;
    Scene(Scene&&) = delete;
    Scene& operator=(Scene&&) = delete;

    [[nodiscard]] static
    lux::cxx::expected<std::unique_ptr<Scene>, SceneBuildFailure>
    create(
        std::shared_ptr<const world::WorldDescription> world,
        std::shared_ptr<const simulation::SimulationDescription> simulation,
        const simulation::SystemRegistry& systems
    ) noexcept;

    [[nodiscard]] const world::WorldDescription&
    world() const noexcept;

    [[nodiscard]] simulation::ecs::Registry&
    registry() noexcept;

    [[nodiscard]] const simulation::ecs::Registry&
    registry() const noexcept;

    [[nodiscard]] simulation::Simulation&
    simulation() noexcept;

    [[nodiscard]] const simulation::Simulation&
    simulation() const noexcept;

    [[nodiscard]] std::stop_token
    stopToken() const noexcept;

    void requestStop() noexcept;

    ~Scene() noexcept;

private:
    struct Impl;
    explicit Scene(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};
```

不增加：

```text
load()
start()
tick()
render()
stream()
findEntity(WorldObjectId)
findPartitionEntities()
```

---

## 6. Exact creation inputs

`Scene::create()` 只接收：

```text
already-loaded WorldDescription
already-loaded SimulationDescription
SystemRegistry type catalog
```

它不负责 Asset IO。

Product boot path：

```text
load SceneDescription asset
    |
    +-> load WorldDescription asset
    +-> load SimulationDescription asset
    |
    v
enter intended Simulation Lane
    |
    v
Scene::create(world, simulation, system_types)
```

禁止为了 Asset loading 给 Scene 增：

```text
AssetClient
AssetManager
AssetResolver
Process client
```

---

## 7. Stable Registry address and build atomicity

Concrete Systems 可能构造时借用：

```text
Registry&
HierarchyIndex&
other Simulation-owned stable objects
```

因此 create 顺序固定：

1. allocate stable `Scene::Impl`；
2. construct `stop_source`；
3. retain WorldDescription；
4. construct Registry at final address；
5. call `Simulation::create(registry, simulation_description, systems)`；
6. install Simulation into Impl；
7. only then return Scene。

失败时：

```text
all partially built Systems destroyed
Registry destroyed
Scene not published
```

不得先在 stack/temp Registry 中 build Simulation，再 move Registry 到 Scene。

---

## 8. Impl declaration/destruction order

推荐：

```cpp
struct Scene::Impl final
{
    std::stop_source stop;
    std::shared_ptr<const world::WorldDescription> world;
    simulation::ecs::Registry registry;
    simulation::Simulation simulation;

    ~Impl() noexcept
    {
        stop.request_stop();
    }
};
```

C++ reverse field destruction保证：

```text
~Impl body: request stop
Simulation destroyed
Registry destroyed
WorldDescription released
stop source destroyed last
```

关键 invariant：

```text
Simulation/System destruction before Registry
```

如果实际 `Simulation` 需要 delayed construction，可用 `std::optional<Simulation>` / private pointer；不要为此新建 owner class。

---

## 9. Simulation Lane construction requirement

当前 LuxObject 绑定 construction thread affinity。

因此：

> `Scene::create()` 的整个 concrete System construction 必须运行在该 Scene 后续执行的 Simulation Lane。

错误：

```text
Main thread:
    create Scene + LuxObject Systems
Simulation thread:
    execute Systems
```

正确：

```text
Product dispatches final Scene::create onto Simulation Lane
```

单线程游戏：

```text
Main thread == Simulation Lane
```

不需要额外 abstraction。

Scene 自身第一版**不继承 LuxObject**。

---

## 10. Cancellation ownership

Scene 只拥有：

```text
std::stop_source
```

它不拥有 Process scope/async_scope。

`stopToken()` 用于 L3 runtime wiring在启动 Scene-associated asynchronous workflow 时 capture token。

Scene destructor/requestStop 不 join Process。

异步 operation 的安全条件在 `06` 强制：

```text
no borrowed Scene/Registry/System memory after start
```

因此 Scene 可在 requestStop 后销毁；late completion 通过 weak target/receiver lifetime安全丢弃。

---

## 11. Scene runtime identity

Runtime canonical identity：

```text
ecs::Entity
```

Scene 不维护：

```text
WorldObjectId -> Entity
PartitionId -> Entity[]
```

WorldObjectId 可以存在于 World payload/Authoring/cook metadata，但不自动 materialize 成 runtime identity component。

如果 gameplay需要 stable identity index：concrete System/Product自己定义。

---

## 12. No mandatory streaming state

Scene 不持有：

```text
WorldPartitionIndex
loaded partitions
pending loads
pending retire
WorldStreamingSelection
StreamingControlState
```

Concrete System需要时自己拥有。

因此 Scene core也不依赖：

```text
scene/spatial3d
scene/spatial2d
Process
Render
```

---

## 13. No implicit World materialization

Scene creation不自动：

```text
load startup partition
materialize root entities
create camera
create player
create renderer
start physics
```

Concrete Product/System显式决定。

这保证：

```text
empty Scene + empty Registry
```

是合法 runtime state。

---

## 14. No main-loop ownership

Scene 不实现：

```text
run()
frameLoop()
pollEvents()
drainMessages()
advanceTime()
```

Host/Product owns execution topology。

Scene 只提供 state/runtime composition。

---

## 15. Tests

必须覆盖：

```text
SceneDescription asset roundtrip: exactly 2 AssetIds
canonical asset type = lux.scene.description
invalid AssetId rejection
Scene::create unknown SystemTypeId failure
Scene create atomic rollback
Registry final-address stability
Simulation destroyed before Registry
requestStop idempotent
Scene destructor requests stop
Scene non-copyable/non-movable
empty Registry valid
no streaming/index fields
no WorldObjectId->Entity API
no SceneFactory/Context/Services public type
```

### Thread-affinity probe

- create Scene on thread A with a LuxObject System；
- verify System affinity = A；
- ensure intended Simulation execution also occurs on A；
- wrong-lane call triggers existing object/thread contract in diagnostics。

---

## 16. Completion condition

Phase 7 完成后，Phase 8 只能通过：

```text
Scene::stopToken()
WorldDescription shared ownership
Registry access at Simulation Lane safe point
```

与 Scene 交互。

如果 Phase 8 发现需要给 Scene 增 loader/streaming manager/context，视为设计错误，不允许现场扩张 Scene core。
