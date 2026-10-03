#pragma once

#include <lux/engine/function/script/ScriptAbility.hpp>
#include <lux/engine/simulation/scripting/ScriptLocalAsync.hpp>

#include <cstdint>
#include <memory>
#include <utility>

namespace lux::simulation::script
{
    namespace detail
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
        [[nodiscard]] static lux::cxx::expected<ScriptApiInstanceBinding, EScriptApiPrepareError>
        create(std::shared_ptr<void> owner, void* context, void (*revoke)(void*) noexcept) noexcept;
        ~ScriptApiInstanceBinding() noexcept { revoke(); }
        ScriptApiInstanceBinding(const ScriptApiInstanceBinding&) = delete;
        ScriptApiInstanceBinding& operator=(const ScriptApiInstanceBinding&) = delete;
        ScriptApiInstanceBinding& operator=(ScriptApiInstanceBinding&&) = delete;
        ScriptApiInstanceBinding(ScriptApiInstanceBinding&& other) noexcept
            : owner_(std::move(other.owner_)), context_(std::exchange(other.context_, nullptr)),
              revoke_(std::exchange(other.revoke_, nullptr))
        {}
        [[nodiscard]] void* context() const noexcept { return context_; }

    private:
        friend class detail::ScriptInstances;
        ScriptApiInstanceBinding(std::shared_ptr<void> owner, void* context, void (*revoke)(void*) noexcept) noexcept
            : owner_(std::move(owner)), context_(context), revoke_(revoke)
        {}
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

    inline ScriptApiInstanceResult ScriptApiInstanceBinding::create(
        std::shared_ptr<void> owner,
        void* context,
        void (*revoke)(void*) noexcept
    ) noexcept
    {
        const bool is_invalid_binding = !owner || context == nullptr || revoke == nullptr;
        if (is_invalid_binding)
            return lux::cxx::unexpected(EScriptApiPrepareError::INVALID_INSTANCE);
        return ScriptApiInstanceBinding{std::move(owner), context, revoke};
    }

    struct ScriptApiCapabilityPublication final
    {
        lux::script::ScriptApiContractIdView contract;
        std::uint64_t schema_hash{};
        void* context{};
        const void* dispatch{};
        std::uint32_t schema_version{1U};
        std::span<const lux::script::ScriptAbilityErasedMethodBinding> methods;
        ScriptApiInstancePrepare prepare_instance{};
    };

    struct PreparedScriptApiCapability final
    {
        lux::script::ScriptApiContractId contract;
        std::uint64_t schema_hash{};
        void* context{};
        const void* dispatch{};
        std::uint32_t schema_version{1U};
        std::span<const lux::script::ScriptAbilityErasedMethodBinding> methods;
        PreparedLocalAsyncCatalog local_async;
        ScriptApiInstancePrepare prepare_instance{};
    };

    [[nodiscard]] inline ScriptApiCapabilityPublication publishScriptAbility(
        const lux::script::ScriptAbilityBinding& binding,
        ScriptApiInstancePrepare prepare_instance = nullptr
    ) noexcept
    {
        if (!binding.valid())
            return {};
        return {
            binding.description->id,
            binding.description->schema_hash,
            binding.context,
            binding.dispatch,
            binding.description->schema_version,
            binding.erased_methods,
            prepare_instance
        };
    }
} // namespace lux::simulation::script
