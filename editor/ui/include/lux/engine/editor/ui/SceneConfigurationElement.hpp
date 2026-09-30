#pragma once

#include <lux/engine/editor/ui/visibility.h>
#include <lux/engine/editor/metadata/SceneRegistrations.hpp>
#include <lux/engine/editor/metadata/EditorPluginExports.hpp>
#include <lux/engine/scene/SceneDescription.hpp>
#include <lux/engine/world/WorldDataSchemaId.hpp>
#include <lux/engine/ui/Element.hpp>

namespace lux::editor::ui
{
    struct SceneProviderOption final
    {
        std::string_view capability;
        std::string_view name;
    };

    struct SceneConfiguration final
    {
        std::string name;
        std::vector<world::WorldDataSchemaId> schemas;
        std::shared_ptr<const simulation::SimulationDescription> simulation;
        lux::scene::SceneDescription scene;
        system::SystemInstanceId viewport;
    };

    enum class ESceneConfigurationStage : std::uint8_t
    {
        ALL,
        CONTENT,
        SIMULATION,
        SCENE,
        FEATURES,
        RELATIONSHIPS
    };
    enum class ESceneContentPreset : std::uint8_t
    {
        EMPTY,
        TWO_DIMENSIONAL,
        THREE_DIMENSIONAL
    };

    // The caller pins these immutable registrations and provider names for the Element's lifetime.
    // This form has no project, file, document or execution ownership.
    class LUX_EDITOR_UI_PUBLIC SceneConfigurationElement final : public lux::ui::Element
    {
    public:
        SceneConfigurationElement(
            lux::ui::Element& parent,
            lux::ui::ElementId,
            const lux::project::PluginCatalog&,
            const SceneRegistrations&,
            std::span<const ConfigurationEditorRegistration>,
            std::span<const SceneProviderOption>,
            EditorResult<void>& status
        );
        ~SceneConfigurationElement() noexcept override;
        [[nodiscard]] EditorResult<SceneConfiguration> build() noexcept;
        [[nodiscard]] EditorResult<void> applyPreset(ESceneContentPreset) noexcept;
        void setStage(ESceneConfigurationStage) noexcept;

    private:
        lux::ui::SizeHint sizeHintContent() noexcept override;
        lux::ui::SizeHint measureContent(float width) noexcept override;
        void arrangeContent() noexcept override;
        void draw() noexcept override;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
