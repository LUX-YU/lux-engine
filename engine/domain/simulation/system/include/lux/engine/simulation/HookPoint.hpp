#pragma once

#include <lux/cxx/container/SlotMap.hpp>
#include <lux/engine/simulation/HookInvocation.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>

namespace lux::simulation
{
    namespace script
    {
        template <class Signature> class TScriptHookEndpoint;
    }
    enum class EEndpointMutationError : std::uint8_t
    {
        NONE,
        NOT_PREPARED,
        CAPACITY_EXCEEDED,
        INVALID_CALLBACK,
        INVALID_TARGET,
        INVALID_TOKEN,
        DISPATCH_ACTIVE,
        WRITER_ACTIVE,
        PAYLOAD_NOT_OWNED,
    };

    struct EndpointConnectionToken final
    {
        std::uint32_t slot{(std::numeric_limits<std::uint32_t>::max)()};
        std::uint32_t generation{};

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return slot != (std::numeric_limits<std::uint32_t>::max)() && generation != 0U;
        }

        friend constexpr bool operator==(EndpointConnectionToken, EndpointConnectionToken) noexcept = default;
    };

    struct EndpointConnectResult final
    {
        EndpointConnectionToken token;
        EEndpointMutationError error{EEndpointMutationError::NONE};

        [[nodiscard]] constexpr explicit operator bool() const noexcept
        {
            return error == EEndpointMutationError::NONE && token.valid();
        }
    };

    template <class Signature> class THookPoint;

    template <class... Parameters> class THookPoint<void(Parameters...)>
    {
    public:
        using Callback = void (*)(void*, Parameters...) noexcept;

        explicit THookPoint(std::size_t handler_capacity) noexcept
            : handlers_(handler_capacity), handler_capacity_(handler_capacity)
        {
        }

        THookPoint(const THookPoint&) = delete;
        THookPoint& operator=(const THookPoint&) = delete;
        THookPoint(THookPoint&&) = delete;
        THookPoint& operator=(THookPoint&&) = delete;
        ~THookPoint() = default;

        [[nodiscard]] EndpointConnectResult connect(void* context, Callback callback) noexcept
        {
            if (dispatch_active_)
            {
                return {{}, EEndpointMutationError::DISPATCH_ACTIVE};
            }
            if (callback == nullptr)
            {
                return {{}, EEndpointMutationError::INVALID_CALLBACK};
            }
            if (handlers_.size() >= handler_capacity_)
            {
                return {{}, EEndpointMutationError::CAPACITY_EXCEEDED};
            }

            const auto inserted = handlers_.emplace(Handler{context, callback});
            return {toToken(inserted), EEndpointMutationError::NONE};
        }

        [[nodiscard]] EEndpointMutationError disconnect(EndpointConnectionToken token) noexcept
        {
            if (dispatch_active_)
            {
                return EEndpointMutationError::DISPATCH_ACTIVE;
            }
            if (!token.valid() || !handlers_.erase(toKey(token)))
            {
                return EEndpointMutationError::INVALID_TOKEN;
            }
            return EEndpointMutationError::NONE;
        }

        [[nodiscard]] std::size_t dispatch(const HookInvocation& invocation, Parameters... parameters) noexcept
        {
            const bool wrong_owner = binding_owner_ != nullptr && invocation.owner_ != binding_owner_;
            const bool wrong_endpoint =
                binding_system_.valid() && (invocation.system_ != binding_system_ || invocation.hook_ != binding_hook_);
            const bool is_non_script_hook = binding_system_.valid() && !invocation.scriptCapable();
            const bool is_invalid_invocation = wrong_owner || wrong_endpoint || is_non_script_hook;
            if (is_invalid_invocation)
            {
                return 0U;
            }
            if (dispatch_active_)
            {
                return 0U;
            }

            dispatch_active_ = true;
            std::size_t calls{};
            for (const auto& handler : handlers_.values())
            {
                handler.callback(handler.context, parameters...);
                ++calls;
            }
            dispatch_active_ = false;
            return calls;
        }

        [[nodiscard]] std::size_t handlerCount() const noexcept
        {
            return handlers_.size();
        }

    private:
        struct HandlerTag;

        struct Handler final
        {
            void* context{};
            Callback callback{};
        };

        using HandlerStorage = lux::cxx::SlotMap<Handler, HandlerTag>;
        using HandlerKey = typename HandlerStorage::key_type;

        [[nodiscard]] static constexpr EndpointConnectionToken toToken(HandlerKey key) noexcept
        {
            return {key.index, key.gen};
        }

        [[nodiscard]] static constexpr HandlerKey toKey(EndpointConnectionToken token) noexcept
        {
            return {token.slot, token.generation};
        }

        HandlerStorage handlers_;
        const std::size_t handler_capacity_;
        bool dispatch_active_{};
        const void* binding_owner_{};
        lux::system::SystemInstanceId binding_system_;
        HookPointId binding_hook_;
        template <class> friend class script::TScriptHookEndpoint;
    };

    template <class... Parameters>
    class THookPoint<void(Parameters...) noexcept> final : public THookPoint<void(Parameters...)>
    {
        using Base = THookPoint<void(Parameters...)>;

    public:
        using Base::Base;
    };
} // namespace lux::simulation
