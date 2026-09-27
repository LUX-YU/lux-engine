#include "MaterialEditorTestAccess.hpp"
#include <lux/engine/editor/material/MaterialEditorImpl.hpp>

namespace lux::editor::material
{
    const lux::material::MaterialSource& MaterialEditorTestAccess::source() const noexcept
    {
        return tool_.impl_->source();
    }

    ProjectStorage& MaterialEditorTestAccess::project() noexcept
    {
        return tool_.impl_->project();
    }

    editing::EditResult<editing::ApplyResult> MaterialEditorTestAccess::rename(std::string_view name)
    {
        return tool_.impl_->rename(std::move(name));
    }

    editing::EditResult<editing::ApplyResult> MaterialEditorTestAccess::setConstant(
        lux::material::NodeId node,
        const std::array<float, 4>& value
    )
    {
        return tool_.impl_->setConstant(std::move(node), value);
    }

    editing::EditResult<editing::ApplyResult> MaterialEditorTestAccess::setShadingModel(
        lux::rdesc::ELightingTechnique value
    )
    {
        return tool_.impl_->setShadingModel(std::move(value));
    }

    editing::EditResult<editing::ApplyResult> MaterialEditorTestAccess::setRenderState(lux::material::RenderState state)
    {
        return tool_.impl_->setRenderState(std::move(state));
    }

    editing::EditResult<editing::ApplyResult> MaterialEditorTestAccess::replaceNode(
        editing::StateId base,
        std::unique_ptr<lux::material::Node>& replacement
    )
    {
        return tool_.impl_->replaceNode(std::move(base), replacement);
    }

    editing::EditResult<editing::ApplyResult> MaterialEditorTestAccess::setTextureSlots(
        editing::StateId base,
        std::span<const lux::material::TextureSlotDecl> slots
    )
    {
        return tool_.impl_->setTextureSlots(std::move(base), std::move(slots));
    }

    editing::EditResult<editing::ApplyResult> MaterialEditorTestAccess::setParameterSlots(
        editing::StateId base,
        std::span<const lux::material::ParamSlotDecl> slots
    )
    {
        return tool_.impl_->setParameterSlots(std::move(base), std::move(slots));
    }

    editing::EditResult<lux::material::NodeId> MaterialEditorTestAccess::insertNode(
        std::unique_ptr<lux::material::Node>& node,
        lux::graph::GraphNodeLayout placement
    )
    {
        return tool_.impl_->insertNode(node, std::move(placement));
    }

    editing::EditResult<editing::ApplyResult> MaterialEditorTestAccess::removeNodes(
        std::span<const lux::material::NodeId> nodes,
        std::span<const lux::graph::LinkRecord> links
    )
    {
        return tool_.impl_->removeNodes(std::move(nodes), std::move(links));
    }

    editing::EditResult<editing::ApplyResult> MaterialEditorTestAccess::connect(
        lux::material::PinId from,
        lux::material::PinId to
    )
    {
        return tool_.impl_->connect(std::move(from), std::move(to));
    }

    editing::EditResult<editing::ApplyResult> MaterialEditorTestAccess::disconnect(
        lux::material::PinId from,
        lux::material::PinId to
    )
    {
        return tool_.impl_->disconnect(std::move(from), std::move(to));
    }

    editing::EditResult<editing::ApplyResult> MaterialEditorTestAccess::moveNodes(
        std::span<const lux::graph::GraphLayoutEntry> entries
    )
    {
        return tool_.impl_->moveNodes(std::move(entries));
    }

    editing::EditResult<editing::ApplyResult> MaterialEditorTestAccess::moveNode(
        lux::material::NodeId node,
        lux::graph::GraphNodeLayout value
    )
    {
        return tool_.impl_->moveNode(std::move(node), std::move(value));
    }

    lux::simulation::ecs::Entity MaterialEditorTestAccess::previewCamera() const noexcept
    {
        return tool_.impl_->previewCamera();
    }

    std::string MaterialEditorTestAccess::previewStatus() const
    {
        return tool_.impl_->previewStatus();
    }

    EditorResult<void> MaterialEditorTestAccess::navigatePreview(
        const lux::simulation::ecs::Transform3D& pose,
        const lux::scene::Camera& camera
    )
    {
        return tool_.impl_->navigatePreview(pose, camera);
    }

    lux::scene::SceneInstanceId MaterialEditorTestAccess::previewInstance() const noexcept
    {
        return tool_.impl_->previewInstance();
    }
}
