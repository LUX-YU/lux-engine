#pragma once

#include <lux/engine/editor/scene/ConfigurationEditor.hpp>
#include <lux/engine/editor/scene/InspectorComponent.hpp>
#include <lux/engine/services/ServiceDescriptor.hpp>
#include <vector>

namespace lux::editor::scene
{
    // Domain declaration input is frozen once. A lazily created catalog shares this backing; neither
    // the generic contribution table nor ServiceRegistry interprets component/configuration fields.
    class SceneEditorCatalog final
    {
    public:
        struct Definition final
        {
            std::vector<ConfigurationEditor> configurations;
            std::vector<InspectorComponent> components;
        };
        explicit SceneEditorCatalog(std::shared_ptr<const Definition> definition) noexcept;
        SceneEditorCatalog(const SceneEditorCatalog&) = delete;
        SceneEditorCatalog& operator=(const SceneEditorCatalog&) = delete;
        SceneEditorCatalog(SceneEditorCatalog&&) = delete;
        SceneEditorCatalog& operator=(SceneEditorCatalog&&) = delete;
        [[nodiscard]] const Definition& definition() const noexcept;
        [[nodiscard]] static services::ServiceResult<std::unique_ptr<SceneEditorCatalog>>
        create(services::ServiceResolver&, const services::ServiceConfiguration&) noexcept;
        [[nodiscard]] static constexpr services::ServiceDescriptor descriptor(services::ServiceNameView implementation)
        {
            auto result = services::ServiceDescriptor::forType<SceneEditorCatalog, &SceneEditorCatalog::create>(
                implementation,
                std::span{&contract_, 1}
            );
            result.definition_type = cxx::typeToken<Definition>();
            return result;
        }

    private:
        static constexpr services::ServiceContract contract_ =
            services::ServiceContract::forType<SceneEditorCatalog, SceneEditorCatalog>(
                services::ServiceNameView{"lux.editor.scene.editors"}
            );
        std::shared_ptr<const Definition> definition_;
    };

    [[nodiscard]] std::shared_ptr<const SceneEditorCatalog::Definition> freezeSceneEditors(
        object::CodeLease,
        SceneEditorCatalog::Definition
    );
    // Cold metadata reads only; callers retain the returned immutable backing, not a mutable catalog.
    [[nodiscard]] services::ServiceResult<std::vector<std::shared_ptr<const SceneEditorCatalog::Definition>>>
        sceneEditorDefinitions(std::span<const std::shared_ptr<const services::ServiceEntry>>) noexcept;
    // Runs inside the original reflection publication scope before any live catalog is swapped.
    [[nodiscard]] services::ServiceResult<void> validateSceneEditors(
        meta::ReflectionRegistry&,
        std::span<const std::shared_ptr<const services::ServiceEntry>>,
        std::size_t capacity
    ) noexcept;
} // namespace lux::editor::scene
