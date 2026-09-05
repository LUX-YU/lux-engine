#pragma once

#include <lux/cxx/container/SlotMap.hpp>
#include <lux/engine/simulation/HookInvocation.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <type_traits>

namespace lux::simulation
{
    namespace script { template <class Signature> class ScriptHookEndpoint; }
    enum class EEndpointMutationError : std::uint8_t
    {
        NONE,
        NOT_PREPARED,
        CAPACITY_EXCEEDED,
        ALLOCATION_FAILURE,
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

    template <class Signature>
    class HookPoint;

    template <class... Parameters>
    class HookPoint<void(Parameters...)>
    {
    public:
        using Callback = void (*)(void *, Parameters...) noexcept;

        HookPoint() = default;
        HookPoint(const HookPoint &) = delete;
        HookPoint &operator=(const HookPoint &) = delete;
        HookPoint(HookPoint &&) = delete;
        HookPoint &operator=(HookPoint &&) = delete;
        ~HookPoint() = default;

        [[nodiscard]] EEndpointMutationError prepare(std::size_t handler_capacity) noexcept
        {
            if (dispatch_active_)
                return EEndpointMutationError::DISPATCH_ACTIVE;

            try
            {
                handlers_.clear();
                handlers_.reserve(handler_capacity);
                handler_capacity_ = handler_capacity;
                prepared_ = true;
                return EEndpointMutationError::NONE;
            }
            catch (const std::bad_alloc &)
            {
                prepared_ = false;
                return EEndpointMutationError::ALLOCATION_FAILURE;
            }
        }

        [[nodiscard]] EndpointConnectResult connect(void *context, Callback callback) noexcept
        {
            if (!prepared_)
                return {{}, EEndpointMutationError::NOT_PREPARED};
            if (dispatch_active_)
                return {{}, EEndpointMutationError::DISPATCH_ACTIVE};
            if (callback == nullptr)
                return {{}, EEndpointMutationError::INVALID_CALLBACK};
            if (handlers_.size() >= handler_capacity_)
                return {{}, EEndpointMutationError::CAPACITY_EXCEEDED};

            const auto inserted = handlers_.tryEmplace(Handler{context, callback});
            if (!inserted)
                return {{}, EEndpointMutationError::ALLOCATION_FAILURE};
            return {toToken(*inserted), EEndpointMutationError::NONE};
        }

        [[nodiscard]] EEndpointMutationError disconnect(EndpointConnectionToken token) noexcept
        {
            if (!prepared_)
                return EEndpointMutationError::NOT_PREPARED;
            if (dispatch_active_)
                return EEndpointMutationError::DISPATCH_ACTIVE;
            if (!token.valid() || !handlers_.erase(toKey(token)))
                return EEndpointMutationError::INVALID_TOKEN;
            return EEndpointMutationError::NONE;
        }

        [[nodiscard]] std::size_t dispatch(const HookInvocation& invocation, Parameters... parameters) noexcept
        {
            const bool wrong_owner = binding_owner_ != nullptr && invocation.owner_ != binding_owner_;
            const bool wrong_endpoint = binding_system_.valid() &&
                (invocation.system_ != binding_system_ || invocation.hook_ != binding_hook_);
            if (wrong_owner || wrong_endpoint || (binding_system_.valid() && !invocation.scriptCapable()))
                return 0U;
            if (!prepared_ || dispatch_active_)
                return 0U;

            dispatch_active_ = true;
            std::size_t calls{};
            for (const auto &handler : handlers_.values())
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
            void *context{};
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
        std::size_t handler_capacity_{};
        bool prepared_{};
        bool dispatch_active_{};
        const void* binding_owner_{};
        lux::system::SystemInstanceId binding_system_;
        HookPointId binding_hook_;
        friend struct detail::HookInvocationTestAccess;
        template <class> friend class script::ScriptHookEndpoint;
    };

    template <class... Parameters>
    class HookPoint<void(Parameters...) noexcept> final : public HookPoint<void(Parameters...)>
    {
    };
}
