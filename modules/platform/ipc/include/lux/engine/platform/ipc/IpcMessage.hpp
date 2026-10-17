#pragma once

#include <cassert>
#include <lux/cxx/compile_time/type_info.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/platform/ipc/visibility.h>

namespace lux::platform::ipc
{
    enum class EIpcReceive : std::uint8_t { RECEIVED, EMPTY, CLOSED };
    enum class EIpcError { INVALID_ARGUMENT, ALREADY_HAS_RESPONDER, NOT_READY, CLOSED, WOULD_BLOCK, INVALID_SOURCE };

    template <typename T>
    using Result = cxx::expected<T, EIpcError>;

    // Same process, same controlled C++ build. The host keeps originating modules loaded until
    // their objects and move/destructor call stacks have ended. IPC owns objects, not module lifetime.
    class alignas(16) LUX_PLATFORM_IPC_PUBLIC IpcMessage final
    {
    public:
        static constexpr std::size_t typed_inline_capacity = 80;

        IpcMessage() noexcept = default;
        ~IpcMessage() noexcept;
        IpcMessage(IpcMessage&& other) noexcept;
        IpcMessage& operator=(IpcMessage&& other) noexcept;
        IpcMessage(const IpcMessage&) = delete;
        IpcMessage& operator=(const IpcMessage&) = delete;

        template <typename T>
        static constexpr bool stores_inline = sizeof(T) <= typed_inline_capacity && alignof(T) <= 16 &&
            std::is_nothrow_move_constructible_v<T>;

        template <typename T, typename... Args>
        requires std::is_nothrow_constructible_v<T, Args...>
        T& emplace(Args&&... args) noexcept
        {
            static_assert(std::is_nothrow_destructible_v<T>);
            reset();
            if constexpr (stores_inline<T>)
                std::construct_at(reinterpret_cast<T*>(object_), std::forward<Args>(args)...);
            else
            {
                auto* value = new T(std::forward<Args>(args)...);
                std::construct_at(reinterpret_cast<void**>(object_), static_cast<void*>(value));
            }
            operations_ = &operations<T>();
            return get<T>();
        }

        [[nodiscard]] bool empty() const noexcept { return operations_ == nullptr; }

        // Precondition: this message contains a live T. No dynamic type or ABI negotiation occurs.
        template <typename T>
        [[nodiscard]] T& get() noexcept
        {
            assert(typeHash() == cxx::type_hash<T>());
            if constexpr (stores_inline<T>) return *std::launder(reinterpret_cast<T*>(object_));
            else return *static_cast<T*>(*std::launder(reinterpret_cast<void**>(object_)));
        }

        template <typename T>
        [[nodiscard]] const T& get() const noexcept
        {
            assert(typeHash() == cxx::type_hash<T>());
            if constexpr (stores_inline<T>) return *std::launder(reinterpret_cast<const T*>(object_));
            else return *static_cast<const T*>(*std::launder(reinterpret_cast<void* const*>(object_)));
        }

        [[nodiscard]] std::uint64_t typeHash() const noexcept
        {
            return operations_ ? operations_->type_hash : 0;
        }

        void reset() noexcept;

    private:
        struct MessageTypeOps final
        {
            std::uint64_t type_hash;
            void (*move)(void*, void*) noexcept;
            void (*destroy)(void*) noexcept;
        };

        template <typename T>
        [[nodiscard]] static const MessageTypeOps& operations() noexcept
        {
            static constexpr MessageTypeOps value{
                cxx::type_hash<T>(),
                [](void* target, void* source) noexcept {
                    if constexpr (stores_inline<T>)
                    {
                        auto* input = std::launder(static_cast<T*>(source));
                        std::construct_at(static_cast<T*>(target), std::move(*input));
                        std::destroy_at(input);
                    }
                    else
                    {
                        auto* input = std::launder(static_cast<void**>(source));
                        std::construct_at(static_cast<void**>(target), *input);
                        std::destroy_at(input);
                    }
                },
                [](void* object) noexcept {
                    if constexpr (stores_inline<T>) std::destroy_at(std::launder(static_cast<T*>(object)));
                    else
                    {
                        auto* pointer = std::launder(static_cast<void**>(object));
                        delete static_cast<T*>(*pointer);
                        std::destroy_at(pointer);
                    }
                }
            };
            return value;
        }

        const MessageTypeOps* operations_ = nullptr;
        alignas(16) std::byte object_[typed_inline_capacity];
    };
}
