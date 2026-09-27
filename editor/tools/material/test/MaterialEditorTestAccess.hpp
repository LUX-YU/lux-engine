#pragma once
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/ui/Element.hpp>
#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/editor/metadata/SceneRegistrations.hpp>
#include <lux/engine/scene/RenderResources.hpp>
#include <lux/engine/scene/SceneInstanceId.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/scene/Camera.hpp>
#include <lux/engine/material/graph/MaterialSource.hpp>
#include <lux/engine/editor/material/MaterialEditor.hpp>

namespace lux::editor::material
{
    class LUX_EDITOR_MATERIAL_PUBLIC MaterialEditorTestAccess final
    {
    public:
        explicit MaterialEditorTestAccess(MaterialEditor& tool) noexcept : tool_(tool) {}
        [[nodiscard]] lux::simulation::ecs::Entity previewCamera() const noexcept;
        [[nodiscard]] std::string previewStatus() const;
        [[nodiscard]] EditorResult<void>
        navigatePreview(const lux::simulation::ecs::Transform3D&, const lux::scene::Camera&);
        [[nodiscard]] lux::scene::SceneInstanceId previewInstance() const noexcept;
        [[nodiscard]] const lux::material::MaterialSource& source() const noexcept;
        [[nodiscard]] ProjectStorage& project() noexcept;
        [[nodiscard]] editing::EditResult<editing::ApplyResult> rename(std::string_view);
        [[nodiscard]] editing::EditResult<editing::ApplyResult>
        setConstant(lux::material::NodeId, const std::array<float, 4>&);
        [[nodiscard]] editing::EditResult<editing::ApplyResult> setShadingModel(lux::rdesc::ELightingTechnique);
        [[nodiscard]] editing::EditResult<editing::ApplyResult> setRenderState(lux::material::RenderState);
        [[nodiscard]] editing::EditResult<editing::ApplyResult>
        replaceNode(editing::StateId, std::unique_ptr<lux::material::Node>&);
        [[nodiscard]] editing::EditResult<
            editing::ApplyResult> setTextureSlots(editing::StateId, std::span<const lux::material::TextureSlotDecl>);
        [[nodiscard]] editing::EditResult<
            editing::ApplyResult> setParameterSlots(editing::StateId, std::span<const lux::material::ParamSlotDecl>);
        [[nodiscard]] editing::EditResult<lux::material::NodeId> insertNode(
            std::unique_ptr<lux::material::Node>&,
            lux::graph::GraphNodeLayout = {}
        );
        [[nodiscard]] editing::EditResult<editing::ApplyResult> removeNodes(
            std::span<const lux::material::NodeId>,
            std::span<const lux::graph::LinkRecord> links = {}
        );
        [[nodiscard]] editing::EditResult<editing::ApplyResult> connect(
            lux::material::PinId from,
            lux::material::PinId to
        );
        [[nodiscard]] editing::EditResult<editing::ApplyResult> disconnect(
            lux::material::PinId from,
            lux::material::PinId to
        );
        [[nodiscard]] editing::EditResult<
            editing::ApplyResult> moveNodes(std::span<const lux::graph::GraphLayoutEntry>);
        [[nodiscard]] editing::EditResult<editing::ApplyResult> moveNode(
            lux::material::NodeId,
            lux::graph::GraphNodeLayout
        );

    private:
        MaterialEditor& tool_;
    };
}
inline auto toolTest(lux::editor::material::MaterialEditor& tool) noexcept
{
    return lux::editor::material::MaterialEditorTestAccess{tool};
}
inline auto toolTest(const lux::editor::material::MaterialEditor& tool) noexcept
{
    return lux::editor::material::MaterialEditorTestAccess{const_cast<lux::editor::material::MaterialEditor&>(tool)};
}
