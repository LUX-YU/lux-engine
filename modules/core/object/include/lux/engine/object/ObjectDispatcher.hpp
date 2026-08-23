#pragma once

#include <cstddef>
#include <memory>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>

#include <lux/engine/core/visibility.h>

namespace lux::object
{
    enum class EPostStatus
    {
        POSTED,
        CLOSED
    };

    class ObjectMessage;

    namespace detail
    {
        struct ObjectMessageQueueState;

        template <class Callable>
        [[nodiscard]] ObjectMessage makeObjectMessage(Callable&& callable);
    } // namespace detail

    /** A move-only, typed transport envelope. It is not a general executor API. */
    class LUX_CORE_PUBLIC ObjectMessage final
    {
    public:
        ObjectMessage() noexcept = default;
        ObjectMessage(ObjectMessage&& other) noexcept;
        ObjectMessage& operator=(ObjectMessage&& other) noexcept;
        ~ObjectMessage();

        ObjectMessage(const ObjectMessage&) = delete;
        ObjectMessage& operator=(const ObjectMessage&) = delete;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return ops_ != nullptr;
        }

    private:
        template <class Callable>
        friend ObjectMessage detail::makeObjectMessage(Callable&& callable);
        friend class ObjectDispatcherRef;
        friend class ObjectMessageQueue;

        struct Ops final
        {
            void (*invoke)(void*) noexcept;
            void (*destroy)(void*) noexcept;
            void (*move_inline)(void*, void*) noexcept;
        };

        static constexpr std::size_t kInlineBytes = 64;

        template <class Callable, bool Inline>
        [[nodiscard]] static const Ops& ops() noexcept
        {
            static const Ops value{
                [](void* storage) noexcept
                {
                    try
                    {
                        (*static_cast<Callable*>(storage))();
                    }
                    catch (...)
                    {
                        std::terminate();
                    }
                },
                [](void* storage) noexcept
                {
                    if constexpr (Inline)
                        std::destroy_at(static_cast<Callable*>(storage));
                    else
                        delete static_cast<Callable*>(storage);
                },
                [](void* source, void* destination) noexcept
                {
                    if constexpr (Inline)
                    {
                        std::construct_at(
                            static_cast<Callable*>(destination),
                            std::move(*static_cast<Callable*>(source))
                        );
                        std::destroy_at(static_cast<Callable*>(source));
                    }
                }
            };
            return value;
        }

        template <class Callable> explicit ObjectMessage(Callable&& callable)
        {
            using Stored = std::remove_cvref_t<Callable>;
            constexpr bool inline_value =
                sizeof(Stored) <= kInlineBytes &&
                alignof(Stored) <= alignof(std::max_align_t) &&
                std::is_nothrow_move_constructible_v<Stored>;
            if constexpr (inline_value)
            {
                data_ = storage_;
                std::construct_at(
                    static_cast<Stored*>(data_),
                    std::forward<Callable>(callable)
                );
            }
            else
            {
                data_ = new Stored(std::forward<Callable>(callable));
            }
            inline_ = inline_value;
            ops_ = std::addressof(ops<Stored, inline_value>());
        }

        void invoke() noexcept;
        void reset() noexcept;
        void moveFrom(ObjectMessage&& other) noexcept;

        alignas(std::max_align_t) std::byte storage_[kInlineBytes]{};
        void* data_{nullptr};
        const Ops* ops_{nullptr};
        bool inline_{false};
    };

    namespace detail
    {
        template <class Callable> ObjectMessage makeObjectMessage(Callable&& callable)
        {
            static_assert(std::is_invocable_v<std::remove_reference_t<Callable>&>);
            return ObjectMessage{std::forward<Callable>(callable)};
        }
    } // namespace detail

    /** Copyable queue capability that stays closed-safe after its provider dies. */
    class LUX_CORE_PUBLIC ObjectDispatcherRef final
    {
    public:
        ObjectDispatcherRef() noexcept = default;

        [[nodiscard]] bool isCurrent() const noexcept;
        [[nodiscard]] EPostStatus post(ObjectMessage&& message) const noexcept;
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return state_ != nullptr;
        }
        [[nodiscard]] bool
        operator==(const ObjectDispatcherRef&) const noexcept = default;

    private:
        friend class ObjectMessageQueue;
        explicit ObjectDispatcherRef(
            std::shared_ptr<detail::ObjectMessageQueueState> state
        ) noexcept
            : state_(std::move(state))
        {
        }

        std::shared_ptr<detail::ObjectMessageQueueState> state_;
    };

    /** Concrete queue provider owned by a session, event loop, or test harness. */
    class LUX_CORE_PUBLIC ObjectMessageQueue final
    {
    public:
        ObjectMessageQueue();
        ~ObjectMessageQueue();

        ObjectMessageQueue(const ObjectMessageQueue&) = delete;
        ObjectMessageQueue& operator=(const ObjectMessageQueue&) = delete;

        [[nodiscard]] ObjectDispatcherRef dispatcherRef() const noexcept;
        [[nodiscard]] std::size_t dispatchPending();
        void close() noexcept;

    private:
        std::shared_ptr<detail::ObjectMessageQueueState> state_;
    };
} // namespace lux::object
