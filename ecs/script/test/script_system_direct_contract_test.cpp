#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/script/components/ScriptComponent.hpp>
#include <lux/engine/ecs/script/systems/ScriptRegistry.hpp>
#include <lux/engine/ecs/script/systems/ScriptSystem.hpp>
#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/resource/asset/script/ScriptAsset.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
    struct CounterBehavior final : lux::ecs::ScriptBehavior
    {
        static inline std::uint32_t creates{0};
        static inline std::uint32_t updates{0};
        static inline std::uint32_t destroys{0};
        static inline float accumulated_dt{0.0f};

        void onCreate() noexcept
        {
            ++creates;
        }

        void onUpdate(float dt) noexcept
        {
            ++updates;
            accumulated_dt += dt;
        }

        void onDestroy() noexcept
        {
            ++destroys;
        }
    };

    struct BackendState final
    {
        lux::asset::AssetManager* manager{nullptr};
        lux::asset::asset_id_t id{};
        std::uint32_t* calls{nullptr};
        std::uint32_t* drops{nullptr};
        bool* referenced_when_dropped{nullptr};
        bool fail{false};
    };

    int invokeBackend(lux_script_call_frame* frame) noexcept
    {
        auto& state = *static_cast<BackendState*>(frame->user_context);
        ++*state.calls;
        return state.fail ? 23 : 0;
    }

    void dropBackend(void* opaque) noexcept
    {
        auto* state = static_cast<BackendState*>(opaque);
        *state->referenced_when_dropped = state->manager->isReferenced(state->id);
        ++*state->drops;
        delete state;
    }

    class TestBackend final : public lux::ecs::IScriptBackend
    {
    public:
        TestBackend(
            lux::asset::AssetManager& manager,
            lux::asset::asset_id_t failing_id,
            std::uint32_t& successful_calls,
            std::uint32_t& failing_calls,
            std::uint32_t& drops,
            bool& success_ref_on_drop,
            bool& failure_ref_on_drop
        ) noexcept
            : manager_(&manager)
            , failing_id_(failing_id)
            , successful_calls_(&successful_calls)
            , failing_calls_(&failing_calls)
            , drops_(&drops)
            , success_ref_on_drop_(&success_ref_on_drop)
            , failure_ref_on_drop_(&failure_ref_on_drop)
        {}

        [[nodiscard]] lux::rdesc::Script::Kind kind() const noexcept override
        {
            return lux::rdesc::Script::Kind::LuaSource;
        }

        void resetSession() noexcept override
        {
            ++resets;
        }

        lux::ecs::ScriptInstance createInstanceFromAsset(
            lux::ecs::EntityHandle,
            lux::ecs::World&,
            const lux::rdesc::Script&,
            std::span<const std::byte>,
            lux::asset::asset_id_t id,
            std::uint32_t
        ) override
        {
            const bool fail = id == failing_id_;
            auto* state = new BackendState{
                manager_,
                id,
                fail ? failing_calls_ : successful_calls_,
                drops_,
                fail ? failure_ref_on_drop_ : success_ref_on_drop_,
                fail
            };
            lux::ecs::ScriptInstance instance(state, &dropBackend);
            instance.bind(
                lux::ecs::ScriptEventRegistry::kOnUpdate,
                lux::ecs::BoundScriptCall{&invokeBackend, state}
            );
            return instance;
        }

        std::uint32_t resets{0};

    private:
        lux::asset::AssetManager* manager_;
        lux::asset::asset_id_t failing_id_;
        std::uint32_t* successful_calls_;
        std::uint32_t* failing_calls_;
        std::uint32_t* drops_;
        bool* success_ref_on_drop_;
        bool* failure_ref_on_drop_;
    };

    std::unique_ptr<lux::asset::ScriptAsset> scriptAsset(
        lux::asset::asset_id_t id,
        lux::rdesc::Script::Body body
    )
    {
        auto info = std::make_unique<lux::asset::AssetInfo>();
        info->id = id;
        info->type = lux::asset::EAssetType::SCRIPT;
        auto script = std::make_unique<lux::rdesc::Script>();
        script->module_name = "direct-contract";
        script->body = std::move(body);
        return std::make_unique<lux::asset::ScriptAsset>(
            std::move(info),
            std::move(script),
            std::vector<std::byte>{std::byte{'x'}}
        );
    }

    lux::ecs::Entity addScript(
        lux::ecs::World& world,
        lux::asset::asset_id_t id
    )
    {
        const auto entity = world.createEntity();
        lux::ecs::ScriptComponent component;
        component.script = id;
        world.emplace<lux::ecs::ScriptComponent>(entity, std::move(component));
        return entity;
    }
}

int main()
{
    static_assert(!std::is_polymorphic_v<lux::ecs::ScriptBehavior>);
    static_assert(!std::is_polymorphic_v<CounterBehavior>);

    lux::asset::AssetManager manager{
        lux::asset::runtimeAssetCodecCatalog()
    };
    const auto cpp_id = manager.generateUUID();
    const auto success_id = manager.generateUUID();
    const auto failure_id = manager.generateUUID();
    assert(manager.registerAsset(scriptAsset(
        cpp_id,
        lux::rdesc::CppBehaviorScript{"Counter"}
    )));
    assert(manager.registerAsset(scriptAsset(
        success_id,
        lux::rdesc::LuaSourceScript{}
    )));
    assert(manager.registerAsset(scriptAsset(
        failure_id,
        lux::rdesc::LuaSourceScript{}
    )));

    lux::ecs::ScriptRegistry registry;
    registry.registerCppScript<CounterBehavior>("Counter");
    lux::ecs::World world;
    const auto cpp_entity = addScript(world, cpp_id);
    const auto success_entity = addScript(world, success_id);
    const auto failure_entity = addScript(world, failure_id);

    std::uint32_t successful_calls = 0;
    std::uint32_t failing_calls = 0;
    std::uint32_t drops = 0;
    bool success_ref_on_drop = false;
    bool failure_ref_on_drop = false;
    TestBackend backend{
        manager,
        failure_id,
        successful_calls,
        failing_calls,
        drops,
        success_ref_on_drop,
        failure_ref_on_drop
    };

    {
        lux::ecs::ScriptSystem scripts(
            registry,
            lux::ecs::ScriptContext{&world, nullptr, nullptr, &manager},
            {&backend}
        );
        scripts.onRuntimeStart(world.registry());
        assert(CounterBehavior::creates == 1);
        assert(manager.isReferenced(cpp_id));
        assert(manager.isReferenced(success_id));
        assert(manager.isReferenced(failure_id));

        scripts.update({world.registry(), 0.25f});
        assert(CounterBehavior::updates == 1);
        assert(CounterBehavior::accumulated_dt == 0.25f);
        assert(successful_calls == 1);
        assert(failing_calls == 1);
        assert(!world.get<lux::ecs::ScriptComponent>(failure_entity).enabled);
        assert(world.get<lux::ecs::ScriptComponent>(success_entity).enabled);

        scripts.update({world.registry(), 0.5f});
        assert(CounterBehavior::updates == 2);
        assert(CounterBehavior::accumulated_dt == 0.75f);
        assert(successful_calls == 2);
        assert(failing_calls == 1);
    }

    assert(CounterBehavior::destroys == 1);
    assert(drops == 2);
    assert(success_ref_on_drop);
    assert(failure_ref_on_drop);
    assert(!manager.isReferenced(cpp_id));
    assert(!manager.isReferenced(success_id));
    assert(!manager.isReferenced(failure_id));
    assert(world.valid(cpp_entity));
    assert(backend.resets == 0);
    return 0;
}
