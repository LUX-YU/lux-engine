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

    private:
        std::shared_ptr<const Definition> definition_;
    };

    [[nodiscard]] std::shared_ptr<const services::ServiceEntry> declareSceneEditors(
        object::CodeLease,
        services::ServiceNameView implementation,
        SceneEditorCatalog::Definition
    );
    // Cold metadata reads only; callers retain the returned immutable backing, not a mutable catalog.
    [[nodiscard]] services::ServiceResult<std::vector<std::shared_ptr<const SceneEditorCatalog::Definition>>>
        sceneEditorDefinitions(std::span<const std::shared_ptr<const services::ServiceEntry>>) noexcept;
    // Runs inside the original reflection publication scope before any live catalog is swapped.
    [[nodiscard]] services::ServiceResult<void>
    validateSceneEditors(meta::ReflectionRegistry&, std::span<const std::shared_ptr<const services::ServiceEntry>>) noexcept;
} // namespace lux::editor::scene
