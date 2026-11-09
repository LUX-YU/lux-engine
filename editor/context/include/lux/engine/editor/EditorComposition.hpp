#pragma once
#include <lux/engine/editor/ContextErrors.hpp>
#include <lux/engine/editor/EditorFacilities.hpp>
#include <lux/engine/editor/EditorServices.hpp>
#include <lux/engine/editor/EditorUiRegistry.hpp>
#include <lux/engine/editor/SceneProfileRegistry.hpp>
#include <lux/engine/editor/ValueEditors.hpp>
#include <lux/engine/object/LuxObject.hpp>

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
            EditorServices::Entry entry{cxx::make_basic_type_info<T>(), std::move(erased)};
            if constexpr (requires(T& value) {
                              { value.initializeProject() } noexcept -> std::same_as<exec::task<FrameworkResult<void>>>;
                          })
            {
                entry.initialize = +[](void* value) noexcept { return static_cast<T*>(value)->initializeProject(); };
            }
            if constexpr (requires(T& value, asset::AssetId scene) {
                              {
                                  value.initializeStartupScene(scene)
                              } noexcept -> std::same_as<exec::task<FrameworkResult<void>>>;
                          })
            {
                entry.initialize_startup_scene = +[](void* value, asset::AssetId scene) noexcept
                { return static_cast<T*>(value)->initializeStartupScene(scene); };
            }
            if constexpr (requires(T& value) {
                              { value.closeAsync() } noexcept -> std::same_as<exec::task<FrameworkResult<void>>>;
                          })
            {
                entry.close = +[](void* value) noexcept { return static_cast<T*>(value)->closeAsync(); };
                if constexpr (requires(T& value) {
                                  { value.requestClose() } noexcept -> std::same_as<void>;
                              })
                {
                    entry.request_close = +[](void* value) noexcept { static_cast<T*>(value)->requestClose(); };
                }
                if constexpr (requires(T& value) {
                                  { value.recoverClose() } noexcept -> std::same_as<exec::task<FrameworkResult<void>>>;
                              })
                {
                    entry.recover = +[](void* value) noexcept { return static_cast<T*>(value)->recoverClose(); };
                }
                else
                {
                    return cxx::unexpected(error::Error{Errors::EditorIncompleteServiceLifecycle});
                }
            }
            if constexpr (std::derived_from<T, object::LuxObject>)
            {
                entry.event = +[](void* value, object::EventView& event) noexcept
                { return object::sendEvent(*static_cast<T*>(value), event); };
            }
            return registerErased(std::move(entry));
        }

        template <class T> [[nodiscard]] cxx::expected<void, EEditorFacilityError> bindExtension(T& surface) noexcept
        {
            return extensions_.bind(surface);
        }

        [[nodiscard]] FrameworkResult<void> registerUiFactory(std::string, UiFactory) noexcept;
        [[nodiscard]] FrameworkResult<void> registerSceneProfile(SceneProfileRegistration) noexcept;
        [[nodiscard]] FrameworkResult<void> registerComponentEditor(ComponentEditor) noexcept;
        [[nodiscard]] FrameworkResult<void> registerSceneConfigurationEditor(SceneConfigurationEditor) noexcept;

    private:
        friend class EditorContext;
        [[nodiscard]] FrameworkResult<void> registerErased(EditorServices::Entry) noexcept;
        EditorFacilities::Composition extensions_;
        std::vector<EditorServices::Entry> services_;
        std::vector<EditorUiRegistry::Factory> ui_;
        std::vector<SceneProfileRegistration> scene_profiles_;
        std::vector<ComponentEditor> component_editors_;
        std::vector<SceneConfigurationEditor> scene_configuration_editors_;
    };
} // namespace lux::editor
