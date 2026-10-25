#pragma once
#include <lux/engine/ContextExtensions.hpp>
#include <lux/engine/editor/ContextErrors.hpp>
#include <lux/engine/editor/EditorServices.hpp>
#include <lux/engine/editor/EditorUiRegistry.hpp>
#include <lux/engine/editor/SceneProfileRegistry.hpp>
#include <lux/engine/editor/ValueEditors.hpp>

namespace lux::editor
{
    // Build-only declarations. No Context, service instance or runtime lookup exists during assembly.
    class EditorComposition final
    {
    public:
        EditorComposition() = default;
        EditorComposition(const EditorComposition&) = delete;
        EditorComposition& operator=(const EditorComposition&) = delete;
        EditorComposition(EditorComposition&&) = delete;
        EditorComposition& operator=(EditorComposition&&) = delete;

        template <class T>
        [[nodiscard]] FrameworkResult<void> registerServiceFactory(
            cxx::move_only_function<FrameworkResult<std::unique_ptr<T>>(EditorContext&) noexcept> factory
        ) noexcept
        {
            if (!factory)
            {
                return cxx::unexpected(error::Error{Errors::EditorEmptyServiceFactory, {}});
            }
            using Owner = EditorServices::Owner;
            using ErasedResult = FrameworkResult<Owner>;
            auto erased = [factory = std::move(factory)](EditorContext& context) mutable noexcept -> ErasedResult
            {
                auto created = factory(context);
                if (!created)
                {
                    return cxx::unexpected(std::move(created.error()));
                }
                if (!*created)
                {
                    return cxx::unexpected(error::Error{Errors::EditorNullService, {}});
                }
                return Owner{created->release(), [](void* value) noexcept { delete static_cast<T*>(value); }};
            };
            return registerErased(cxx::typeToken<T>(), std::move(erased));
        }

        template <class T>
        [[nodiscard]] cxx::expected<void, engine::EContextExtensionError> bindExtension(T& surface) noexcept
        {
            return extensions_.bind(surface);
        }

        [[nodiscard]] FrameworkResult<void> registerUiFactory(std::string, UiFactory) noexcept;
        [[nodiscard]] FrameworkResult<void> registerSceneProfile(SceneProfileRegistration) noexcept;
        [[nodiscard]] FrameworkResult<void> registerComponentEditor(ComponentEditor) noexcept;
        [[nodiscard]] FrameworkResult<void> registerSceneConfigurationEditor(SceneConfigurationEditor) noexcept;

    private:
        friend class EditorContext;
        [[nodiscard]] FrameworkResult<void> registerErased(cxx::TypeToken, EditorServices::Factory) noexcept;
        engine::ContextExtensions::Composition extensions_;
        std::vector<EditorServices::Entry> services_;
        std::vector<EditorUiRegistry::Factory> ui_;
        std::vector<SceneProfileRegistration> scene_profiles_;
        std::vector<ComponentEditor> component_editors_;
        std::vector<SceneConfigurationEditor> scene_configuration_editors_;
    };
} // namespace lux::editor
