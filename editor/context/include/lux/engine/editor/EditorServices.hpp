#pragma once
#include <exec/task.hpp>
#include <functional>
#include <lux/cxx/compile_time/type_info.hpp>
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/editor/FrameworkResult.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/resource/identity/AssetId.hpp>
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
        [[nodiscard]] exec::task<FrameworkResult<void>> initializeProject(EditorContext&) noexcept;
        void requestClose() noexcept;
        [[nodiscard]] bool dispatch(object::EventView&) noexcept;
        [[nodiscard]] bool closeComplete() const noexcept;
        [[nodiscard]] exec::task<FrameworkResult<void>> closeAsync() noexcept;
        [[nodiscard]] exec::task<FrameworkResult<void>> recoverClose() noexcept;
        EditorServices(const EditorServices&) = delete;
        EditorServices& operator=(const EditorServices&) = delete;
        EditorServices(EditorServices&&) = delete;
        EditorServices& operator=(EditorServices&&) = delete;

        template <class T> [[nodiscard]] FrameworkResult<std::reference_wrapper<T>> get(EditorContext& context) noexcept
        {
            auto result = getErased(cxx::make_basic_type_info<T>(), context);
            if (!result)
            {
                return cxx::unexpected(result.error());
            }
            return std::ref(*static_cast<T*>(*result));
        }

        // Composition/teardown query; never constructs a service or reports a missing optional product tool.
        template <class T> [[nodiscard]] T* constructed() noexcept
        {
            for (const auto& entry : entries_)
            {
                if (entry.type == cxx::make_basic_type_info<T>())
                {
                    return static_cast<T*>(entry.instance.get());
                }
            }
            return nullptr;
        }

    private:
        friend class EditorContext;
        friend class EditorComposition;
        using Owner = std::unique_ptr<void, void (*)(void*) noexcept>;
        using Factory = cxx::move_only_function<FrameworkResult<Owner>(EditorContext&) noexcept>;

        struct Entry final
        {
            cxx::type_info type;
            Factory factory;
            Owner instance{nullptr, nullptr};
            bool constructing{};
            exec::task<FrameworkResult<void>> (*initialize)(void*) noexcept = nullptr;
            exec::task<FrameworkResult<void>> (*initialize_startup_scene)(void*, asset::AssetId) noexcept = nullptr;
            bool initialized = false;
            void (*request_close)(void*) noexcept = nullptr;
            exec::task<FrameworkResult<void>> (*close)(void*) noexcept = nullptr;
            exec::task<FrameworkResult<void>> (*recover)(void*) noexcept = nullptr;
            bool (*event)(void*, object::EventView&) noexcept = nullptr;
            bool close_complete = false;
        };

        explicit EditorServices(std::vector<Entry>) noexcept;
        [[nodiscard]] FrameworkResult<void*> getErased(cxx::type_info, EditorContext&) noexcept;
        std::vector<Entry> entries_;
        std::vector<std::size_t> construction_order_;
        bool closing_ = false;
        bool initializing_ = false;
    };
} // namespace lux::editor
