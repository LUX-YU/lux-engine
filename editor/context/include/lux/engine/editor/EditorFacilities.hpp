#pragma once

#include <lux/cxx/compile_time/type_info.hpp>
#include <lux/cxx/compile_time/expected.hpp>
#include <type_traits>
#include <utility>
#include <vector>

namespace lux::editor
{
    enum class EEditorFacilityError
    {
        DUPLICATE_TYPE,
        TYPE_COLLISION
    };

    // A published table only borrows already-created domain surfaces. Their owners must
    // outlive the Context. Neither publication nor lookup constructs or retains a service.
    class EditorFacilities final
    {
        struct Entry final
        {
            cxx::type_info type;
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
            [[nodiscard]] cxx::expected<void, EEditorFacilityError> bind(T& surface) noexcept
            {
                const auto type = cxx::make_basic_type_info<T>();
                for (const auto& entry : entries_)
                {
                    if (entry.type == type)
                    {
                        return cxx::unexpected(EEditorFacilityError::DUPLICATE_TYPE);
                    }
                    if (entry.type.hash() == type.hash())
                    {
                        return cxx::unexpected(EEditorFacilityError::TYPE_COLLISION);
                    }
                }
                entries_.push_back({type, &surface});
                return {};
            }

            [[nodiscard]] EditorFacilities publish() && noexcept
            {
                return EditorFacilities{std::move(entries_)};
            }

        private:
            std::vector<Entry> entries_;
        };

        EditorFacilities(const EditorFacilities&) = delete;
        EditorFacilities& operator=(const EditorFacilities&) = delete;
        EditorFacilities(EditorFacilities&&) = delete;
        EditorFacilities& operator=(EditorFacilities&&) = delete;

        template <class T> [[nodiscard]] T* find() noexcept
        {
            const auto* table = this;
            return const_cast<T*>(table->find<T>());
        }

        template <class T> [[nodiscard]] const T* find() const noexcept
        {
            const auto type = cxx::make_basic_type_info<T>();
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
        explicit EditorFacilities(std::vector<Entry> entries) noexcept : entries_(std::move(entries)) {}

        std::vector<Entry> entries_;
    };
} // namespace lux::editor
