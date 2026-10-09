#pragma once

#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/flowforge/FlowForgeFailure.hpp>
#include <lux/engine/flowforge/visibility.h>
#include <lux/engine/object/CodeLease.hpp>

#include <memory>
#include <type_traits>
#include <utility>

namespace lux::flowforge
{
    // Owns only semantic data and its code. No node/pin identity or graph membership.
    class LUX_ENGINE_FLOWFORGE_PUBLIC FlowNodePayload final
    {
    public:
        FlowNodePayload() noexcept = default;
        ~FlowNodePayload();
        FlowNodePayload(const FlowNodePayload&) = delete;
        FlowNodePayload& operator=(const FlowNodePayload&) = delete;
        FlowNodePayload(FlowNodePayload&&) noexcept;
        FlowNodePayload& operator=(FlowNodePayload&&) noexcept;

        template <class T, auto Clone, class... Args>
            requires std::is_nothrow_constructible_v<T, Args...> && std::is_nothrow_destructible_v<T> &&
                     std::is_same_v<decltype(Clone), FlowForgeResult<std::unique_ptr<T>> (*)(const T&) noexcept>
        [[nodiscard]] static FlowForgeResult<FlowNodePayload> make(object::CodeLease code, Args&&... args) noexcept
        {
            static_assert(Clone != nullptr);
            if (!code.valid())
            {
                return cxx::unexpected(
                    FlowForgeFailure{EFlowForgeError::INVALID_DESCRIPTION, "node payload requires a valid code lease"}
                );
            }
            auto value = std::make_unique<T>(std::forward<Args>(args)...);
            return FlowNodePayload{
                std::move(code),
                cxx::typeToken<T>(),
                value.release(),
                [](void* pointer) noexcept { delete static_cast<T*>(pointer); },
                [](const void* pointer) noexcept -> FlowForgeResult<void*>
                {
                    auto copy = Clone(*static_cast<const T*>(pointer));
                    if (!copy)
                    {
                        return cxx::unexpected(std::move(copy.error()));
                    }
                    return copy->release();
                }
            };
        }

        [[nodiscard]] FlowForgeResult<FlowNodePayload> clone() const noexcept;

        template <class T> [[nodiscard]] T* get() noexcept
        {
            return type_ == cxx::typeToken<T>() ? static_cast<T*>(value_) : nullptr;
        }

        template <class T> [[nodiscard]] const T* get() const noexcept
        {
            return type_ == cxx::typeToken<T>() ? static_cast<const T*>(value_) : nullptr;
        }

    private:
        friend class FlowNodeType;
        using Destroy = void (*)(void*) noexcept;
        using Clone = FlowForgeResult<void*> (*)(const void*) noexcept;

        FlowNodePayload(object::CodeLease, cxx::TypeToken, void*, Destroy, Clone) noexcept;
        void reset() noexcept;

        object::CodeLease code_{object::CodeLease::builtin()};
        cxx::TypeToken type_;
        void* value_{};
        Destroy destroy_{};
        Clone clone_{};
    };
} // namespace lux::flowforge
