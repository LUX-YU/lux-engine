#include <lux/engine/ecs/script/backends/NativeModuleScriptBackend.hpp>

#include <lux/engine/function/script/native/NativeModule.hpp>

#include <algorithm>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace lux::ecs
{
    namespace
    {
        bool sameType(
            const lux_script_type_desc& loaded,
            const lux::rdesc::ScriptValueType& manifest
        ) noexcept
        {
            return loaded.kind == manifest.kind
                && loaded.size == manifest.size
                && loaded.type_id == manifest.type_id;
        }

        bool sameType(
            const lux_script_type_desc& loaded,
            const lux_script_type_desc& event
        ) noexcept
        {
            return loaded.kind == event.kind
                && loaded.size == event.size
                && loaded.type_id == event.type_id;
        }

        bool matchesManifest(
            const lux_script_function_desc& loaded,
            const lux::rdesc::ScriptFunction& manifest
        ) noexcept
        {
            if (loaded.arg_count != manifest.args.size()
                || loaded.return_count != manifest.returns.size())
                return false;
            for (std::size_t i = 0; i < manifest.args.size(); ++i)
                if (!sameType(loaded.args[i], manifest.args[i]))
                    return false;
            for (std::size_t i = 0; i < manifest.returns.size(); ++i)
                if (!sameType(loaded.returns[i], manifest.returns[i]))
                    return false;
            return true;
        }

        bool matchesEvent(
            const lux_script_function_desc& loaded,
            const ScriptEventDesc& event
        ) noexcept
        {
            if (loaded.return_count != 0
                || loaded.arg_count != event.abi_params.size())
                return false;
            for (std::size_t i = 0; i < event.abi_params.size(); ++i)
                if (!sameType(loaded.args[i], event.abi_params[i]))
                    return false;
            return true;
        }
    }

    struct NativeModuleScriptBackend::Impl
    {
        struct ModuleEntry
        {
            lux::script::NativeModule module;
            std::vector<const lux_script_function_desc*> event_functions;
            std::vector<std::byte> state_defaults;
            std::uint32_t state_size{0};
            std::uint32_t content_revision{0};
        };

        struct InstanceState
        {
            std::vector<std::byte> block;
        };

        HostSymbolResolver resolver;
        std::unordered_map<lux::asset::asset_id_t, ModuleEntry> modules;

        explicit Impl(HostSymbolResolver host_resolver)
            : resolver(std::move(host_resolver))
        {}

        lux::script::HostSymbolResolver makeResolver()
        {
            return lux::script::HostSymbolResolver(
                [this](std::string_view symbol) -> void*
                {
                    return resolver ? resolver(symbol) : nullptr;
                }
            );
        }

        static void dropInstance(void* state) noexcept
        {
            delete static_cast<InstanceState*>(state);
        }
    };

    NativeModuleScriptBackend::NativeModuleScriptBackend(
        HostSymbolResolver resolver
    )
        : impl_(std::make_unique<Impl>(std::move(resolver)))
    {}

    NativeModuleScriptBackend::~NativeModuleScriptBackend() = default;

    ScriptInstance NativeModuleScriptBackend::createInstanceFromAsset(
        EntityHandle,
        World&,
        const lux::rdesc::Script& description,
        std::span<const std::byte> payload,
        lux::asset::asset_id_t asset_id,
        std::uint32_t content_revision
    )
    {
        const auto* native = std::get_if<lux::rdesc::NativeModuleScript>(
            &description.body
        );
        if (!native || asset_id.is_nil() || payload.empty()
            || native->abi_version != LUX_SCRIPT_ABI_VERSION
            || native->state_defaults.size() > native->state_size)
            return {};

        auto module_it = impl_->modules.find(asset_id);
        if (module_it == impl_->modules.end())
        {
            auto loaded = lux::script::loadNativeModule(
                payload,
                description.module_name.empty()
                    ? std::string_view{"native_script"}
                    : std::string_view{description.module_name},
                impl_->makeResolver()
            );
            if (!loaded)
                return {};

            std::vector<const lux_script_function_desc*> event_functions(
                scriptEventRegistry().count(),
                nullptr
            );

            if (loaded.value().functions().size() != native->functions.size())
                return {};

            for (const auto& manifest : native->functions)
            {
                const auto* function = loaded.value().findFunction(manifest.name);
                if (!function || !matchesManifest(*function, manifest))
                    return {};
            }

            for (ScriptEventId id = 0; id < scriptEventRegistry().count(); ++id)
            {
                const auto& event = scriptEventRegistry().desc(id);
                const auto* function = loaded.value().findFunction(event.name);
                if (!function)
                    continue;
                const auto manifest = std::find_if(
                    native->functions.begin(),
                    native->functions.end(),
                    [&](const auto& item) { return item.name == event.name; }
                );
                if (manifest == native->functions.end()
                    || !matchesManifest(*function, *manifest)
                    || !matchesEvent(*function, event))
                    return {};
                event_functions[id] = function;
            }

            module_it = impl_->modules.emplace(
                asset_id,
                Impl::ModuleEntry{
                    std::move(loaded.value()),
                    std::move(event_functions),
                    native->state_defaults,
                    native->state_size,
                    content_revision
                }
            ).first;
        }

        auto state = std::make_unique<Impl::InstanceState>();
        state->block.assign(module_it->second.state_size, std::byte{0});
        if (!module_it->second.state_defaults.empty())
        {
            std::memcpy(
                state->block.data(),
                module_it->second.state_defaults.data(),
                module_it->second.state_defaults.size()
            );
        }

        void* context = state->block.empty() ? nullptr : state->block.data();
        ScriptInstance instance(state.get(), &Impl::dropInstance);
        for (ScriptEventId id = 0;
             id < module_it->second.event_functions.size();
             ++id)
        {
            const auto* function = module_it->second.event_functions[id];
            if (function)
                instance.bind(id, BoundScriptCall{function->invoke, context});
        }
        state.release();
        return instance;
    }

    void NativeModuleScriptBackend::resetSession() noexcept
    {
        impl_->modules.clear();
    }
}
