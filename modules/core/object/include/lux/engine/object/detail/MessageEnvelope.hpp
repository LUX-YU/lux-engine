#pragma once

#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#include <lux/engine/core/visibility.h>

namespace lux::object
{
    class ObjectMessageQueue;

    namespace detail
    {
#if defined(LUX_OBJECT_TEST_DIAGNOSTICS)
        LUX_CORE_PUBLIC void recordMessageStorageForTest(bool inline_value) noexcept;
        LUX_CORE_PUBLIC void resetMessageStorageForTest() noexcept;
        [[nodiscard]] LUX_CORE_PUBLIC std::size_t inlineMessageStorageCountForTest() noexcept;
        [[nodiscard]] LUX_CORE_PUBLIC std::size_t heapMessageStorageCountForTest() noexcept;
#endif

        class LUX_CORE_PUBLIC MessageEnvelope final
        {
          public:
            MessageEnvelope() noexcept = default;
            MessageEnvelope(MessageEnvelope &&other) noexcept;
            MessageEnvelope &operator=(MessageEnvelope &&other) noexcept;
            ~MessageEnvelope();

            MessageEnvelope(const MessageEnvelope &) = delete;
            MessageEnvelope &operator=(const MessageEnvelope &) = delete;

          private:
            template <class Callable> friend MessageEnvelope makeMessage(Callable &&callable);
            friend class ::lux::object::ObjectMessageQueue;

            struct Ops final
            {
                void (*invoke)(void *) noexcept;
                void (*destroy)(void *) noexcept;
                void (*move_inline)(void *, void *) noexcept;
            };

            static constexpr std::size_t kInlineBytes = 64;

            template <class Callable, bool Inline> [[nodiscard]] static const Ops &ops() noexcept
            {
                static const Ops value{[](void *storage) noexcept {
                                           try
                                           {
                                               (*static_cast<Callable *>(storage))();
                                           }
                                           catch (...)
                                           {
                                               std::terminate();
                                           }
                                       },
                                       [](void *storage) noexcept {
                                           if constexpr (Inline)
                                               std::destroy_at(static_cast<Callable *>(storage));
                                           else
                                               delete static_cast<Callable *>(storage);
                                       },
                                       [](void *source, void *destination) noexcept {
                                           if constexpr (Inline)
                                           {
                                               std::construct_at(
                                                   static_cast<Callable *>(destination),
                                                   std::move(*static_cast<Callable *>(source)));
                                               std::destroy_at(static_cast<Callable *>(source));
                                           }
                                       }};
                return value;
            }

            template <class Callable> explicit MessageEnvelope(Callable &&callable)
            {
                using Stored = std::remove_cvref_t<Callable>;
                constexpr bool inline_value = sizeof(Stored) <= kInlineBytes &&
                                              alignof(Stored) <= alignof(std::max_align_t) &&
                                              std::is_nothrow_move_constructible_v<Stored>;
                if constexpr (inline_value)
                {
                    data_ = storage_;
                    std::construct_at(static_cast<Stored *>(data_),
                                      std::forward<Callable>(callable));
                }
                else
                {
                    data_ = new Stored(std::forward<Callable>(callable));
                }
                inline_ = inline_value;
                ops_ = std::addressof(ops<Stored, inline_value>());
#if defined(LUX_OBJECT_TEST_DIAGNOSTICS)
                recordMessageStorageForTest(inline_value);
#endif
            }

            void invoke() noexcept;
            void reset() noexcept;
            void moveFrom(MessageEnvelope &&other) noexcept;

            alignas(std::max_align_t) std::byte storage_[kInlineBytes]{};
            void *data_{nullptr};
            const Ops *ops_{nullptr};
            bool inline_{false};
        };

        template <class Callable> MessageEnvelope makeMessage(Callable &&callable)
        {
            static_assert(std::is_invocable_v<std::remove_reference_t<Callable> &>);
            return MessageEnvelope{std::forward<Callable>(callable)};
        }
    } // namespace detail
} // namespace lux::object
