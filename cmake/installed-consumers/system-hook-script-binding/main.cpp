#include "ConsumerBehavior.hpp"

#include <lux/engine/resource/asset/script/ScriptAsset.hpp>
#include <lux/engine/simulation/CppStaticScriptBridge.hpp>
#include <lux/engine/simulation/SimulationAssetCodec.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <limits>
#include <memory>
#include <span>
#include <string_view>

namespace
{
    using namespace lux::simulation;

    inline constexpr std::array kHooks{
        makeSystemHookPoint<void(float)>(
            "value-a",
            ESystemHookCardinality::MULTI
        ),
        makeSystemHookPoint<void(float)>(
            "value-b",
            ESystemHookCardinality::MULTI
        )};
    inline constexpr std::array kEvents{
        makeSystemEvent<std::int32_t>(
            "pulse",
            kHooks[0],
            ESystemEventTarget::ENTITY_TARGETED,
            "lux.i32",
            1U
        )};
    inline constexpr SystemDescription kSystem{
        .canonical_name = "consumer.system",
        .version = 1U,
        .hooks = kHooks,
        .events = kEvents};

    struct Asset final
    {
        lux::asset::AssetId id;
        lux::asset::ScriptAssetContent content;
    };

    bool resolveAsset(
        void* opaque,
        const lux::asset::AssetId& id,
        ResolvedScriptAsset& result
    ) noexcept
    {
        auto& asset = *static_cast<Asset*>(opaque);
        if (asset.id != id)
            return false;
        result.asset = std::addressof(asset.content);
        return true;
    }

    bool resolveRecord(
        void*,
        const lux::meta::RefType& type,
        lux::script::ScriptSemanticLayout& result
    ) noexcept
    {
        if (type.hash != lux::cxx::type_hash<EBehaviorStopReason>())
            return false;
        result = {
            lux::script::scriptSemanticTypeId(
                BehaviorStopReasonCanonicalName
            ),
            BehaviorStopReasonCanonicalName,
            LUX_SCRIPT_VK_UINT32,
            sizeof(EBehaviorStopReason),
            alignof(EBehaviorStopReason)};
        return true;
    }

    ScriptBindingDescription lifecycle(
        lux::script::ScriptSymbolId symbol,
        EBehaviorLifecyclePoint point
    )
    {
        return {symbol, BehaviorLifecycleBindingTarget{point}};
    }

    lux::asset::AssetCodecLimits unlimited()
    {
        return {
            std::numeric_limits<std::size_t>::max(),
            std::numeric_limits<std::size_t>::max(),
            std::numeric_limits<std::size_t>::max()};
    }
}

int main()
{
    using namespace lux::simulation;
    using installed_consumer::ConsumerBehavior;

    lux::meta::ReflectionRegistry::initRegistry();
    {
        auto& reflection = lux::meta::ReflectionRegistry::instance();
        const auto* reflected = reflection.findClass(
            "installed_consumer::ConsumerBehavior"
        );
        assert(reflected && reflected->methods.size() == 5U);
        std::array<const lux::meta::RefMethod*, 5U> methods{};
        for (std::size_t index{}; index < methods.size(); ++index)
            methods[index] = std::addressof(reflected->methods[index]);
        auto projected = projectCppStaticEntityScript<ConsumerBehavior>(
            "consumer.behavior",
            "consumer-behavior-v1",
            *reflected,
            methods,
            CppStaticRecordSemanticResolver{nullptr, &resolveRecord}
        );
        assert(projected);

        auto find_symbol = [&](std::string_view name)
        {
            for (const auto& function : projected->description().exports)
            {
                if (function.name == name)
                    return function.symbol_id;
            }
            return lux::script::InvalidScriptSymbolId;
        };
        const auto value_symbol = find_symbol("onValue");
        const auto event_symbol = find_symbol("onEvent");
        assert(value_symbol && event_symbol);

        std::array<std::uint8_t, 16U> id_bytes{};
        id_bytes[0] = 0xC4U;
        Asset asset{lux::asset::AssetId{id_bytes}, {}};
        asset.content.description = projected->description();

        const auto script_codec = lux::asset::scriptAssetCodecDescriptor({});
        const auto encoded_script = script_codec.encode(
            std::addressof(asset.content),
            lux::asset::AssetEncodeContext{unlimited()}
        );
        assert(encoded_script && (*encoded_script)[4] == std::byte{2U});
        const auto decoded_script = script_codec.decode(
            *encoded_script,
            lux::asset::AssetDecodeContext{unlimited()}
        );
        assert(decoded_script);
        asset.content = *std::static_pointer_cast<
            const lux::asset::ScriptAssetContent>(decoded_script->payload);

        ScriptMountDescription mount{
            ScriptMountId{9U},
            asset.id,
            {
                lifecycle(
                    find_symbol("construct"),
                    EBehaviorLifecyclePoint::CONSTRUCT
                ),
                lifecycle(
                    find_symbol("start"),
                    EBehaviorLifecyclePoint::START
                ),
                lifecycle(
                    find_symbol("stop"),
                    EBehaviorLifecyclePoint::STOP
                ),
                {value_symbol, SystemHookBindingTarget{
                    systemTypeId(kSystem.canonical_name),
                    "consumer",
                    "value-a"}},
                {value_symbol, SystemHookBindingTarget{
                    systemTypeId(kSystem.canonical_name),
                    "consumer",
                    "value-b"}},
                {event_symbol, SystemEventBindingTarget{
                    systemTypeId(kSystem.canonical_name),
                    "consumer",
                    "pulse"}},
            }};

        SimulationDescriptionBuilder builder;
        assert(builder.addSystem("consumer", kSystem));
        auto description = std::move(builder).build();
        assert(description);
        const auto simulation_codec = simulationAssetCodecDescriptor({});
        const auto encoded_simulation = simulation_codec.encode(
            std::addressof(*description),
            lux::asset::AssetEncodeContext{unlimited()}
        );
        assert(encoded_simulation &&
            (*encoded_simulation)[4] == std::byte{4U});
        assert(simulation_codec.decode(
            *encoded_simulation,
            lux::asset::AssetDecodeContext{unlimited()}
        ));

        ecs::Registry registry;
        const auto entity = registry.create();
        registry.emplace<ScriptComponent>(
            entity,
            ScriptComponent{{mount}}
        );
        const std::array descriptors{std::addressof(*projected)};
        CppStaticScriptBindingBackend backend{descriptors, 1U};
        assert(backend);
        const auto backend_descriptor = backend.descriptor();
        auto created = ScriptBindingSession::create(
            std::move(*description),
            registry,
            ScriptBindingCapacities{1U, 5U, 4U, 4U, 4U},
            ScriptAssetResolver{&asset, &resolveAsset},
            std::span{&backend_descriptor, 1U}
        );
        assert(created);
        auto session = std::move(*created);
        assert(session.prepare());
        assert(installed_consumer::constructs == 1U);
        assert(installed_consumer::starts == 1U);
        assert(installed_consumer::observed_self == entity);
        assert(session.preparedMethodCount() == 5U);

        float value{2.5F};
        lux_script_value_slot value_slot{
            LUX_SCRIPT_VK_FLOAT,
            {},
            sizeof(value),
            lux::script::scriptSemanticTypeId("lux.f32"),
            &value};
        lux_script_call_frame value_frame{
            &value_slot, 1U, 0U, nullptr, 0U, 0U, nullptr, nullptr};
        assert(session.dispatchHook(
            session.hookSlot("consumer", "value-a"),
            entity,
            value_frame
        ).calls == 1U);
        assert(session.dispatchHook(
            session.hookSlot("consumer", "value-b"),
            entity,
            value_frame
        ).calls == 1U);
        assert(installed_consumer::observed_value == 5.0F);

        std::int32_t pulse{17};
        lux_script_value_slot event_slot{
            LUX_SCRIPT_VK_INT32,
            {},
            sizeof(pulse),
            lux::script::scriptSemanticTypeId("lux.i32"),
            &pulse};
        lux_script_call_frame event_frame{
            &event_slot, 1U, 0U, nullptr, 0U, 0U, nullptr, nullptr};
        assert(session.dispatchEvent(
            session.eventSlot("consumer", "pulse"),
            entity,
            event_frame
        ).calls == 1U);
        assert(installed_consumer::observed_event == pulse);
        assert(session.shutdown());
        assert(installed_consumer::stops == 1U);
    }
    lux::meta::ReflectionRegistry::destroyRegistry();
}
