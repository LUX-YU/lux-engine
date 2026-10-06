#pragma once
#include <functional>
#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/editor/FrameworkError.hpp>
#include <memory>
#include <vector>

namespace lux::editor
{
    class EditorContext;

    class EditorServiceRegistrar final
    {
    public:
        EditorServiceRegistrar() = default;
        ~EditorServiceRegistrar();
        EditorServiceRegistrar(const EditorServiceRegistrar&) = delete;
        EditorServiceRegistrar& operator=(const EditorServiceRegistrar&) = delete;
        EditorServiceRegistrar(EditorServiceRegistrar&&) = delete;
        EditorServiceRegistrar& operator=(EditorServiceRegistrar&&) = delete;

        template <class T>
        [[nodiscard]] FrameworkResult<void> registerFactory(
            cxx::move_only_function<FrameworkResult<std::unique_ptr<T>>(EditorContext&)> factory
        ) noexcept
        {
            if (!factory)
            {
                return cxx::unexpected(FrameworkFailure{EFrameworkError::FACTORY_FAILED, "Empty service factory"});
            }
            auto erased = [factory = std::move(factory)](EditorContext& context) mutable -> FrameworkResult<Owner>
            {
                auto created = factory(context);
                if (!created)
                {
                    return cxx::unexpected(std::move(created.error()));
                }
                if (!*created)
                {
                    return cxx::unexpected(FrameworkFailure{EFrameworkError::FACTORY_FAILED, "Null service"});
                }
                return Owner{created->release(), [](void* value) noexcept { delete static_cast<T*>(value); }};
            };
            return registerErased(cxx::typeToken<T>(), std::move(erased));
        }

        template <class T> [[nodiscard]] FrameworkResult<std::reference_wrapper<T>> get(EditorContext& context) noexcept
        {
            auto result = getErased(cxx::typeToken<T>(), context);
            if (!result)
            {
                return cxx::unexpected(std::move(result.error()));
            }
            return std::ref(*static_cast<T*>(*result));
        }
        void freeze() noexcept
        {
            frozen_ = true;
        }

    private:
        using Owner = std::unique_ptr<void, void (*)(void*) noexcept>;
        using Factory = cxx::move_only_function<FrameworkResult<Owner>(EditorContext&)>;
        struct Entry final
        {
            cxx::TypeToken type;
            Factory factory;
            Owner instance{nullptr, nullptr};
            bool constructing{};
        };
        [[nodiscard]] FrameworkResult<void> registerErased(cxx::TypeToken, Factory) noexcept;
        [[nodiscard]] FrameworkResult<void*> getErased(cxx::TypeToken, EditorContext&) noexcept;
        std::vector<Entry> entries_;
        std::vector<std::size_t> construction_order_;
        bool frozen_{};
        bool closing_{};
    };
} // namespace lux::editor
