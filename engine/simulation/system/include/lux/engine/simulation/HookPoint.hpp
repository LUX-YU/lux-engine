#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace lux::simulation
{
    enum class EEndpointMutationError : std::uint8_t
    {
        NONE,
        NOT_PREPARED,
        CAPACITY_EXCEEDED,
        ALLOCATION_FAILURE,
        INVALID_CALLBACK,
        INVALID_TOKEN,
        DISPATCH_ACTIVE,
        WRITER_ACTIVE,
    };

    struct EndpointConnectionToken final
    {
        std::uint64_t value{};

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return value != 0U;
        }

        friend constexpr bool operator==(
            EndpointConnectionToken,
            EndpointConnectionToken
        ) noexcept = default;
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
        using Callback = void (*)(void*, Parameters...) noexcept;

        HookPoint() = default;
        HookPoint(const HookPoint&) = delete;
        HookPoint& operator=(const HookPoint&) = delete;
        HookPoint(HookPoint&&) = delete;
        HookPoint& operator=(HookPoint&&) = delete;
        ~HookPoint() = default;

        [[nodiscard]] EEndpointMutationError prepare(
            std::size_t handler_capacity,
            std::size_t mutation_capacity
        ) noexcept
        {
            if (dispatch_active_)
                return EEndpointMutationError::DISPATCH_ACTIVE;
            try
            {
                handlers_.clear();
                mutations_.clear();
                handlers_.reserve(handler_capacity);
                mutations_.reserve(mutation_capacity);
                handler_capacity_ = handler_capacity;
                mutation_capacity_ = mutation_capacity;
                next_token_ = 1U;
                prepared_ = true;
                return EEndpointMutationError::NONE;
            }
            catch (const std::bad_alloc&)
            {
                prepared_ = false;
                return EEndpointMutationError::ALLOCATION_FAILURE;
            }
        }

        [[nodiscard]] EndpointConnectResult connect(
            void* context,
            Callback callback
        ) noexcept
        {
            if (!prepared_)
                return {{}, EEndpointMutationError::NOT_PREPARED};
            if (callback == nullptr)
                return {{}, EEndpointMutationError::INVALID_CALLBACK};
            const auto pending_connects = static_cast<std::size_t>(std::count_if(
                mutations_.begin(),
                mutations_.end(),
                [](const Mutation& value) noexcept
                {
                    return value.connect;
                }
            ));
            if (handlers_.size() + pending_connects >= handler_capacity_ ||
                mutations_.size() >= mutation_capacity_)
            {
                return {{}, EEndpointMutationError::CAPACITY_EXCEEDED};
            }
            auto token = EndpointConnectionToken{next_token_++};
            if (!token.valid())
                token = EndpointConnectionToken{next_token_++};
            mutations_.push_back({true, token, context, callback});
            return {token, EEndpointMutationError::NONE};
        }

        [[nodiscard]] EEndpointMutationError disconnect(
            EndpointConnectionToken token
        ) noexcept
        {
            if (!prepared_)
                return EEndpointMutationError::NOT_PREPARED;
            if (!token.valid())
                return EEndpointMutationError::INVALID_TOKEN;
            if (mutations_.size() >= mutation_capacity_)
                return EEndpointMutationError::CAPACITY_EXCEEDED;
            mutations_.push_back({false, token, nullptr, nullptr});
            return EEndpointMutationError::NONE;
        }

        [[nodiscard]] EEndpointMutationError flushMutations() noexcept
        {
            if (!prepared_)
                return EEndpointMutationError::NOT_PREPARED;
            if (dispatch_active_)
                return EEndpointMutationError::DISPATCH_ACTIVE;
            for (const auto& mutation : mutations_)
            {
                if (mutation.connect)
                {
                    handlers_.push_back({
                        mutation.token,
                        mutation.context,
                        mutation.callback,
                        true});
                    continue;
                }
                const auto found = std::find_if(
                    handlers_.begin(),
                    handlers_.end(),
                    [&](const Handler& handler) noexcept
                    {
                        return handler.token == mutation.token;
                    }
                );
                if (found != handlers_.end())
                    found->active = false;
            }
            mutations_.clear();
            handlers_.erase(
                std::remove_if(
                    handlers_.begin(),
                    handlers_.end(),
                    [](const Handler& handler) noexcept
                    {
                        return !handler.active;
                    }
                ),
                handlers_.end()
            );
            return EEndpointMutationError::NONE;
        }

        [[nodiscard]] std::size_t dispatch(Parameters... parameters) noexcept
        {
            if (!prepared_ || dispatch_active_)
                return 0U;
            dispatch_active_ = true;
            std::size_t calls{};
            for (const auto& handler : handlers_)
            {
                if (!handler.active)
                    continue;
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
        struct Handler final
        {
            EndpointConnectionToken token;
            void* context{};
            Callback callback{};
            bool active{};
        };

        struct Mutation final
        {
            bool connect{};
            EndpointConnectionToken token;
            void* context{};
            Callback callback{};
        };

        std::vector<Handler> handlers_;
        std::vector<Mutation> mutations_;
        std::size_t handler_capacity_{};
        std::size_t mutation_capacity_{};
        std::uint64_t next_token_{1U};
        bool prepared_{};
        bool dispatch_active_{};
    };

    template <class... Parameters>
    class HookPoint<void(Parameters...) noexcept> final
        : public HookPoint<void(Parameters...)>
    {
    };
}
