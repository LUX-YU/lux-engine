#pragma once
#include <functional>
#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/editor/FrameworkResult.hpp>
#include <memory>
#include <vector>

namespace lux::editor
{
    class EditorContext;
    class EditorComposition;
    class EditorServices final
    {
    public:
        ~EditorServices() noexcept;
        EditorServices(const EditorServices&) = delete;
        EditorServices& operator=(const EditorServices&) = delete;
        EditorServices(EditorServices&&) = delete;
        EditorServices& operator=(EditorServices&&) = delete;

        template <class T> [[nodiscard]] FrameworkResult<std::reference_wrapper<T>> get(EditorContext& context) noexcept
        {
            auto result = getErased(cxx::typeToken<T>(), context);
            if (!result)
            {
                return cxx::unexpected(result.error());
            }
            return std::ref(*static_cast<T*>(*result));
        }

    private:
        friend class EditorContext;
        friend class EditorComposition;
        using Owner = std::unique_ptr<void, void (*)(void*) noexcept>;
        using Factory = cxx::move_only_function<FrameworkResult<Owner>(EditorContext&) noexcept>;
        struct Entry final
        {
            cxx::TypeToken type;
            Factory factory;
            Owner instance{nullptr, nullptr};
            bool constructing{};
        };
        explicit EditorServices(std::vector<Entry>) noexcept;
        [[nodiscard]] FrameworkResult<void*> getErased(cxx::TypeToken, EditorContext&) noexcept;
        std::vector<Entry> entries_;
        std::vector<std::size_t> construction_order_;
    };
} // namespace lux::editor
