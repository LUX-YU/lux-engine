#include <lux/engine/simulation/NativeScriptBindingBackend.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <new>
#include <vector>

namespace lux::simulation
{
    struct NativeScriptBindingBackend::State final
    {
        struct Instance final
        {
            std::shared_ptr<lux::script::NativeModule> module;
            std::vector<std::byte> instance_state;
        };

        struct InstanceEntry final
        {
            lux::asset::AssetId script;
            ecs::Entity entity{ecs::NullEntity};
            std::uint32_t mount_ordinal{};
            std::weak_ptr<Instance> instance;
        };

        struct Call final
        {
            std::shared_ptr<Instance> instance;
            const lux_script_function_desc* function{};
        };

        explicit State(
            std::shared_ptr<lux::script::NativeModule> value,
            std::size_t instance_capacity
        )
            : module(std::move(value))
        {
            instances.reserve(instance_capacity);
        }

        static int invoke(lux_script_call_frame* frame) noexcept
        {
            if (!frame || !frame->user_context)
                return -1;
            auto& call = *static_cast<Call*>(frame->user_context);
            frame->user_context = call.instance->instance_state.empty()
                ? nullptr
                : call.instance->instance_state.data();
            return call.function->invoke(frame);
        }

        static EScriptBackendPrepareResult prepare(
            void* opaque,
            const ScriptPrepareContext& context,
            const lux::asset::ScriptAssetContent& asset,
            const lux::rdesc::ScriptFunction& description,
            lux::script::BoundScriptCall& result
        ) noexcept
        {
            auto& self = *static_cast<State*>(opaque);
            const auto* body = std::get_if<lux::rdesc::NativeModuleScript>(
                std::addressof(asset.description.body)
            );
            if (!self.module || !body ||
                body->abi_version != self.module->abiVersion() ||
                body->state_defaults.size() > body->state_size)
            {
                return EScriptBackendPrepareResult::CONSTRUCTION_FAILURE;
            }
            const auto* function = self.module->findFunction(
                description.symbol_id
            );
            if (!function || function->arg_count != description.args.size() ||
                function->return_count != description.returns.size())
            {
                return EScriptBackendPrepareResult::SIGNATURE_MISMATCH;
            }
            const auto same = [](const lux_script_type_desc& native_type,
                                 const lux::rdesc::ScriptValueType& semantic)
                noexcept
            {
                return native_type.name &&
                    native_type.type_id == semantic.type_id &&
                    semantic.canonical_name == native_type.name;
            };
            for (std::size_t index{}; index < description.args.size(); ++index)
            {
                if (!same(function->args[index], description.args[index]))
                    return EScriptBackendPrepareResult::SIGNATURE_MISMATCH;
            }
            for (std::size_t index{}; index < description.returns.size(); ++index)
            {
                if (!same(function->returns[index], description.returns[index]))
                    return EScriptBackendPrepareResult::SIGNATURE_MISMATCH;
            }
            try
            {
                auto call = std::make_unique<Call>();
                call->function = function;
                for (auto& entry : self.instances)
                {
                    if (entry.script == context.script &&
                        entry.entity == context.entity &&
                        entry.mount_ordinal == context.mount_ordinal)
                    {
                        call->instance = entry.instance.lock();
                        break;
                    }
                }
                if (!call->instance)
                {
                    self.instances.erase(
                        std::remove_if(
                            self.instances.begin(),
                            self.instances.end(),
                            [](const InstanceEntry& entry) noexcept
                            {
                                return entry.instance.expired();
                            }
                        ),
                        self.instances.end()
                    );
                    if (self.instances.size() >= self.instances.capacity())
                    {
                        return EScriptBackendPrepareResult::CAPACITY_EXCEEDED;
                    }
                    call->instance = std::make_shared<Instance>();
                    call->instance->module = self.module;
                    call->instance->instance_state.resize(body->state_size);
                    std::copy(
                        body->state_defaults.begin(),
                        body->state_defaults.end(),
                        call->instance->instance_state.begin()
                    );
                    self.instances.push_back(InstanceEntry{
                        context.script,
                        context.entity,
                        context.mount_ordinal,
                        call->instance});
                }
                result = lux::script::BoundScriptCall{
                    &State::invoke,
                    call.release()};
                return EScriptBackendPrepareResult::SUCCESS;
            }
            catch (const std::bad_alloc&)
            {
                return EScriptBackendPrepareResult::ALLOCATION_FAILURE;
            }
        }

        static void release(
            void*,
            lux::script::BoundScriptCall call
        ) noexcept
        {
            delete static_cast<Call*>(call.context);
        }

        std::shared_ptr<lux::script::NativeModule> module;
        std::vector<InstanceEntry> instances;
    };

    NativeScriptBindingBackend::NativeScriptBindingBackend(
        std::shared_ptr<lux::script::NativeModule> module,
        std::size_t instance_capacity
    ) noexcept
    {
        try
        {
            state_ = std::make_unique<State>(
                std::move(module),
                instance_capacity
            );
        }
        catch (const std::bad_alloc&)
        {}
    }

    NativeScriptBindingBackend::~NativeScriptBindingBackend() = default;
    NativeScriptBindingBackend::NativeScriptBindingBackend(
        NativeScriptBindingBackend&&
    ) noexcept = default;
    NativeScriptBindingBackend& NativeScriptBindingBackend::operator=(
        NativeScriptBindingBackend&&
    ) noexcept = default;

    NativeScriptBindingBackend::operator bool() const noexcept
    {
        return state_ && state_->module;
    }

    ScriptBackendDescriptor NativeScriptBindingBackend::descriptor() noexcept
    {
        return ScriptBackendDescriptor{
            lux::rdesc::Script::Kind::NATIVE_MODULE,
            state_.get(),
            &State::prepare,
            &State::release};
    }
}
