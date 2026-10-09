#pragma once

#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/cxx/compile_time/expected.hpp>
#include <type_traits>
#include <utility>
#include <vector>

namespace lux::engine
{
    enum class EContextExtensionError
    {
        DUPLICATE_TYPE,
        TYPE_COLLISION
    };

    // A published table only borrows already-created domain surfaces. Their owners must
    // outlive the Context. Neither publication nor lookup constructs or retains a service.
    class ContextExtensions final
    {
        struct Entry final
        {
            cxx::TypeToken type;
            void* surface;
        };

    public:
        class Composition final
        {
        public:
            Composition() = default;
            Composition(const Composition&) = delete;
            Composition& operator=(const Composition&) = delete;
            Composition(Composition&&) noexcept = default;
            Composition& operator=(Composition&&) noexcept = default;

            template <class T>
                requires(!std::is_const_v<T>)
            [[nodiscard]] cxx::expected<void, EContextExtensionError> bind(T& surface) noexcept
            {
                const auto type = cxx::typeToken<T>();
                for (const auto& entry : entries_)
                {
                    if (entry.type == type)
                    {
                        return cxx::unexpected(EContextExtensionError::DUPLICATE_TYPE);
                    }
                    if (entry.type.hash() == type.hash())
                    {
                        return cxx::unexpected(EContextExtensionError::TYPE_COLLISION);
                    }
                }
                entries_.push_back({type, &surface});
                return {};
            }

            [[nodiscard]] ContextExtensions publish() && noexcept
            {
                return ContextExtensions{std::move(entries_)};
            }

        private:
            std::vector<Entry> entries_;
        };

        ContextExtensions(const ContextExtensions&) = delete;
        ContextExtensions& operator=(const ContextExtensions&) = delete;
        ContextExtensions(ContextExtensions&&) = delete;
        ContextExtensions& operator=(ContextExtensions&&) = delete;

        template <class T> [[nodiscard]] T* find() noexcept
        {
            const auto* table = this;
            return const_cast<T*>(table->find<T>());
        }

        template <class T> [[nodiscard]] const T* find() const noexcept
        {
            const auto type = cxx::typeToken<T>();
            for (const auto& entry : entries_)
            {
                if (entry.type == type)
                {
                    return static_cast<const T*>(entry.surface);
                }
            }
            return nullptr;
        }

    private:
        explicit ContextExtensions(std::vector<Entry> entries) noexcept : entries_(std::move(entries)) {}

        std::vector<Entry> entries_;
    };
} // namespace lux::engine
