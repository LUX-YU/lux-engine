from pathlib import Path
s=Path('E:/SyncForder/CodeRepos/lux-engine-ec2')
p=s/'engine/domain/simulation/scripting/core/include/lux/engine/simulation/scripting/ScriptApiCapability.hpp'
t=p.read_text().replace('#include <cstdint>','#include <cstdint>\n#include <memory>\n#include <utility>')
pos=t.index('    struct ScriptApiCapabilityPublication final')
t=t[:pos]+'''    namespace detail
    {
        class ScriptInstances;
    }

    enum class EScriptApiPrepareError : std::uint8_t
    {
        INVALID_INSTANCE,
        CAPACITY_EXCEEDED,
        STOPPING
    };

    // One instance's prepared provider ownership. The mount revokes admission before backend
    // destruction; accepted work may retain native scope data until its original completion settles.
    class ScriptApiInstanceBinding final
    {
    public:
        ScriptApiInstanceBinding(std::shared_ptr<void> owner, void* context, void (*revoke)(void*) noexcept) noexcept
            : owner_(std::move(owner)), context_(context), revoke_(revoke)
        {}
        ~ScriptApiInstanceBinding() noexcept { revoke(); }
        ScriptApiInstanceBinding(const ScriptApiInstanceBinding&) = delete;
        ScriptApiInstanceBinding& operator=(const ScriptApiInstanceBinding&) = delete;
        ScriptApiInstanceBinding& operator=(ScriptApiInstanceBinding&&) = delete;
        ScriptApiInstanceBinding(ScriptApiInstanceBinding&& other) noexcept
            : owner_(std::move(other.owner_)), context_(std::exchange(other.context_, nullptr)),
              revoke_(std::exchange(other.revoke_, nullptr))
        {}
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return owner_ && context_ && revoke_;
        }
        [[nodiscard]] void* context() const noexcept { return context_; }

    private:
        friend class detail::ScriptInstances;
        void revoke() noexcept
        {
            if (const auto callback = std::exchange(revoke_, nullptr))
                callback(context_);
        }
        std::shared_ptr<void> owner_;
        void* context_{};
        void (*revoke_)(void*) noexcept {};
    };
    using ScriptApiInstanceResult = lux::cxx::expected<ScriptApiInstanceBinding, EScriptApiPrepareError>;
    using ScriptApiInstancePrepare = ScriptApiInstanceResult (*)(void*, ScriptInstanceId) noexcept;

'''+t[pos:]
t=t.replace('        std::span<const lux::script::ScriptAbilityErasedMethodBinding> methods;\n    };','        std::span<const lux::script::ScriptAbilityErasedMethodBinding> methods;\n        ScriptApiInstancePrepare prepare_instance{};\n    };')
t=t.replace('        PreparedLocalAsyncCatalog local_async;','        PreparedLocalAsyncCatalog local_async;\n        ScriptApiInstancePrepare prepare_instance{};')
t=t.replace('        const lux::script::ScriptAbilityBinding& binding\n','        const lux::script::ScriptAbilityBinding& binding,\n        ScriptApiInstancePrepare prepare_instance = nullptr\n')
t=t.replace('            binding.erased_methods\n','            binding.erased_methods,\n            prepare_instance\n')
p.write_text(t)
p=s/'engine/domain/simulation/builtin_systems/script/src/ScriptPreparer.cpp';t=p.read_text().replace('                return {};\n            };','                capabilities_.back().prepare_instance = capability.prepare_instance;\n                return {};\n            };',1);p.write_text(t)
p=s/'engine/domain/simulation/builtin_systems/script/pinclude/lux/engine/simulation/script/ScriptInstances.hpp';t=p.read_text().replace('            std::vector<PreparedScriptApiCapability> capabilities;','            std::vector<PreparedScriptApiCapability> capabilities;\n            std::vector<ScriptApiInstanceBinding> capability_instances;');p.write_text(t)
p=s/'engine/domain/simulation/builtin_systems/script/src/ScriptInstances.cpp';t=p.read_text()
t=t.replace('            capabilities.reserve(count);','            capabilities.reserve(count);\n            owner_->mounts_[slot_].capability_instances.reserve(count);')
mark='        mount.invocation->instance = {inserted->index + 1U, inserted->gen};'
t=t.replace(mark,mark+'''
        Protection protection{*owner_};
        for (auto& capability : mount.capabilities)
        {
            if (!capability.prepare_instance)
                continue;
            auto prepared = capability.prepare_instance(capability.context, mount.invocation->instance);
            if (!prepared)
            {
                switch (prepared.error())
                {
                case EScriptApiPrepareError::CAPACITY_EXCEEDED:
                    return lux::cxx::unexpected(EScriptSystemError::CAPACITY_EXCEEDED);
                case EScriptApiPrepareError::STOPPING:
                    return lux::cxx::unexpected(EScriptSystemError::SHUT_DOWN);
                case EScriptApiPrepareError::INVALID_INSTANCE:
                    return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);
                }
            }
            if (!*prepared)
                return lux::cxx::unexpected(EScriptSystemError::INVALID_INPUT);
            capability.context = prepared->context();
            mount.capability_instances.push_back(std::move(*prepared));
        }''')
mark='            mount.invocation->retiring_instance = instance;\n            mount.invocation->instance = {};'
t=t.replace(mark,mark+'''\n            for (auto& capability : mount.capability_instances)
                capability.revoke();''')
t=t.replace('        mount.capabilities.clear();','        mount.capabilities.clear();\n        mount.capability_instances.clear();')
p.write_text(t)
