#include <lux/engine/function/script/native/NativeModule.hpp>
#include <lux/engine/simulation/EventPoint.hpp>
#include <lux/engine/simulation/ecs/Registry.hpp>
#include <lux/engine/simulation/scripting/lua/LuaScriptBackend.hpp>
#include <lux/engine/simulation/scripting/native/NativeScriptBackend.hpp>

#include <lua.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    using namespace lux::simulation;
    using namespace lux::simulation::script;

    struct CollisionEvent final
    {
        std::int32_t body{};
        float impulse{};
    };

    constexpr std::string_view kCollisionName{
        "lux.physics.CollisionEvent"};
    constexpr lux::script::ScriptSymbolId kCollisionSymbol{0xC0111510U};

    struct Subscriber final
    {
        lux::script::BoundScriptCall call;
        std::size_t callbacks{};
    };

    void dispatchSubscriber(
        void* opaque,
        const ecs::Entity&,
        const CollisionEvent& collision
    ) noexcept
    {
        auto& subscriber = *static_cast<Subscriber*>(opaque);
        lux_script_value_slot slot{
            LUX_SCRIPT_VK_STRUCT_REF,
            {},
            sizeof(collision),
            lux::semantic::typeId(kCollisionName),
            const_cast<CollisionEvent*>(std::addressof(collision))};
        lux_script_call_frame frame{
            &slot,
            1U,
            0U,
            nullptr,
            0U,
            0U,
            nullptr,
            subscriber.call.context};
        if (subscriber.call.invoke(&frame) == 0)
            ++subscriber.callbacks;
    }

    bool pushCollision(
        void*,
        void* opaque_state,
        const void* opaque_value
    ) noexcept
    {
        auto* state = static_cast<lua_State*>(opaque_state);
        const auto& collision = *static_cast<const CollisionEvent*>(
            opaque_value
        );
        lua_createtable(state, 0, 2);
        lua_pushinteger(state, collision.body);
        lua_setfield(state, -2, "body");
        lua_pushnumber(state, collision.impulse);
        lua_setfield(state, -2, "impulse");
        return true;
    }

    bool resolveRecord(
        void*,
        std::uint64_t type_id,
        std::string_view canonical_name,
        lux_script_type_desc& result
    ) noexcept
    {
        const bool matches = type_id ==
                lux::semantic::typeId(kCollisionName) &&
            canonical_name == kCollisionName;
        if (!matches)
            return false;
        result = {
            kCollisionName.data(),
            type_id,
            sizeof(CollisionEvent),
            alignof(CollisionEvent),
            LUX_SCRIPT_VK_STRUCT_REF,
            static_cast<std::uint8_t>(
                lux::semantic::EValuePass::CONST_REF
            ),
            {}};
        return true;
    }

    struct ModuleProvider final
    {
        lux::script::NativeModule* module{};

        static bool resolve(
            void* opaque,
            const lux::asset::AssetId&,
            const lux::script::ScriptArtifact&,
            ResolvedNativeModule& result
        ) noexcept
        {
            result.module = static_cast<ModuleProvider*>(opaque)->module;
            return result.module != nullptr;
        }
    };

    [[nodiscard]] lux::rdesc::ScriptValueType collisionType()
    {
        return {
            std::string{kCollisionName},
            lux::semantic::typeId(kCollisionName),
            lux::semantic::EValuePass::CONST_REF,
            LUX_SCRIPT_VK_STRUCT_REF,
            sizeof(CollisionEvent),
            alignof(CollisionEvent)};
    }

    [[nodiscard]] lux::script::ScriptArtifact nativeArtifact(
        const lux::script::NativeModule& module
    )
    {
        lux::rdesc::Script description;
        description.module_name = "native_collision_fixture";
        description.body = lux::rdesc::NativeModuleScript{
            LUX_SCRIPT_ABI_VERSION,
            module.stateLayoutHash(),
            64U,
            64U,
            {}};
        description.exports.push_back({
            "on_collision",
            kCollisionSymbol,
            {collisionType()},
            {}});
        auto artifact = lux::script::ScriptArtifact::create(std::move(description), {});
        assert(artifact);
        return std::move(*artifact);
    }

    [[nodiscard]] lux::script::ScriptArtifact luaArtifact()
    {
        lux::rdesc::Script description;
        description.module_name = "lua_collision_fixture";
        description.body = lux::rdesc::LuaSourceScript{"fixture"};
        description.exports.push_back({
            "on_collision",
            kCollisionSymbol,
            {collisionType()},
            {}});
        constexpr std::string_view source = R"lua(
            return {
                on_collision = function(self, event)
                    if event.body ~= 42 or event.impulse ~= 3.5 then
                        error("collision payload mismatch")
                    end
                    self.collisions = (self.collisions or 0) + 1
                end
            }
        )lua";
        std::vector<std::byte> payload;
        payload.reserve(source.size());
        for (const auto value : source)
            payload.push_back(static_cast<std::byte>(value));
        auto artifact = lux::script::ScriptArtifact::create(std::move(description), std::move(payload));
        assert(artifact);
        return std::move(*artifact);
    }
}

int main()
{
    using namespace lux::simulation;
    using namespace lux::simulation::script;

    auto loaded = lux::script::loadNativeModule(
        std::filesystem::path{LUX_SCRIPT_NATIVE_COLLISION_FIXTURE}
    );
    assert(loaded);
    ModuleProvider provider{std::addressof(*loaded)};
    NativeScriptBackend native_backend{
        {std::addressof(provider), &ModuleProvider::resolve},
        NativeScriptBackendConfig{
            .module_capacity = 1U,
            .instance_capacity = 1U,
            .prepared_call_capacity = 4U,
            .continuation_capacity = 2U,
            .max_ability_imports_per_module = 4U,
            .max_continuation_frame_bytes = 4096U,
            .record_layouts = {nullptr, &resolveRecord}
        }};
    assert(native_backend);

    const LuaRecordMarshaller marshaller{
        lux::semantic::typeId(kCollisionName),
        std::string{kCollisionName},
        sizeof(CollisionEvent),
        alignof(CollisionEvent),
        nullptr,
        &pushCollision};
    auto lua_created = LuaScriptBackend::create(
        {1U, 2U, 1U, 4U, 1U, {}, std::span{&marshaller, 1U}}
    );
    assert(lua_created);
    auto lua_backend = std::move(*lua_created);

    std::array<std::uint8_t, 16U> id_bytes{};
    id_bytes[0] = 0xC0U;
    const lux::asset::AssetId asset_id{id_bytes};
    auto native_asset = nativeArtifact(*loaded);
    auto lua_asset = luaArtifact();

    ecs::Registry registry;
    const auto entity = registry.create();
    ScriptBehavior behavior;
    ScriptBackendInstance native_instance;
    ScriptBackendInstance lua_instance;
    auto native_descriptor = native_backend.descriptor();
    auto lua_descriptor = lua_backend.descriptor();
    const ScriptInstanceCreateContext context{
        asset_id,
        EntityScriptScope{entity},
        &behavior};
    assert(native_descriptor.createInstance(
        native_descriptor.context,
        context,
        native_asset,
        native_instance
    ) == EScriptBackendResult::SUCCESS);
    assert(lua_descriptor.createInstance(
        lua_descriptor.context,
        context,
        lua_asset,
        lua_instance
    ) == EScriptBackendResult::SUCCESS);

    Subscriber native_subscriber;
    Subscriber lua_subscriber;
    assert(native_descriptor.prepareMethod(
        native_descriptor.context,
        native_instance,
        native_asset.description().exports[0],
        native_subscriber.call
    ) == EScriptBackendResult::SUCCESS);
    assert(lua_descriptor.prepareMethod(
        lua_descriptor.context,
        lua_instance,
        lua_asset.description().exports[0],
        lua_subscriber.call
    ) == EScriptBackendResult::SUCCESS);

    EventPoint<EntityTargetedRoute<ecs::Entity>, CollisionEvent> endpoint;
    assert(endpoint.prepare(1U, 2U, 2U) == EEndpointMutationError::NONE);
    const auto native_connection = endpoint.connect(
        entity,
        std::addressof(native_subscriber),
        &dispatchSubscriber
    );
    const auto lua_connection = endpoint.connect(
        entity,
        std::addressof(lua_subscriber),
        &dispatchSubscriber
    );
    assert(native_connection && lua_connection);

    auto writer = endpoint.begin(0U);
    std::thread producer{
        [writer = std::move(writer), entity]() mutable
        {
            assert(writer.record(entity, CollisionEvent{42, 3.5F}));
        }};
    producer.join();
    assert(native_subscriber.callbacks == 0U);
    assert(lua_subscriber.callbacks == 0U);
    assert(endpoint.drain() == 2U);
    assert(native_subscriber.callbacks == 1U);
    assert(lua_subscriber.callbacks == 1U);

    registry.destroy(entity);
    const auto reused = registry.create();
    assert(reused != entity);
    auto reused_writer = endpoint.begin(0U);
    assert(reused_writer.record(reused, CollisionEvent{42, 3.5F}));
    reused_writer = {};
    assert(endpoint.drain() == 0U);
    assert(endpoint.disconnect(native_connection.token) ==
        EEndpointMutationError::NONE);
    assert(endpoint.disconnect(lua_connection.token) ==
        EEndpointMutationError::NONE);

    native_descriptor.releaseMethod(
        native_descriptor.context,
        native_instance,
        native_subscriber.call
    );
    lua_descriptor.releaseMethod(
        lua_descriptor.context,
        lua_instance,
        lua_subscriber.call
    );
    native_descriptor.destroyInstance(
        native_descriptor.context,
        native_instance
    );
    lua_descriptor.destroyInstance(lua_descriptor.context, lua_instance);
    return 0;
}
