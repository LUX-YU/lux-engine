#pragma once
#include <cassert>
#include <lux/engine/editor/Editor.hpp>
#include <lux/engine/editor/scene/SceneEditor.hpp>
#include <lux/engine/editor/material/MaterialEditor.hpp>
#include <lux/engine/editor/flowforge/FlowForgeEditor.hpp>

// Test frontend policy: explicitly discard through the same tool review used by the UI.
class TestExit final
{
public:
    void request(lux::editor::Editor& editor)
    {
        owner_ = &editor;
        editor.requestExit();
        poll();
    }
    void poll()
    {
        using namespace lux::editor;
        if (!owner_ || owner_->closing()) return;
        const auto review = [](auto& tool) {
            if (tool.assetStatus().phase == EAssetEditPhase::REVIEW)
                assert(tool.reviewAsset(EAssetChangeDecision::DISCARD));
            for (const auto request : tool.saveRequests())
            {
                const auto status = tool.saveStatus(request);
                assert(status);
                if (std::holds_alternative<SaveRetryable>(*status)) assert(tool.abandonSave(request));
                else if (std::holds_alternative<SaveSucceeded>(*status) || std::holds_alternative<SaveAbandoned>(*status))
                    static_cast<void>(tool.acknowledgeSave(request));
            }
        };
        for (auto* child = owner_->firstChild(); child; child = child->nextSibling())
        {
            if (auto* tool = dynamic_cast<scene::SceneEditor*>(child)) review(*tool);
            if (auto* tool = dynamic_cast<material::MaterialEditor*>(child)) review(*tool);
            if (auto* tool = dynamic_cast<flowforge::FlowForgeEditor*>(child)) review(*tool);
        }
    }
private:
    lux::editor::Editor* owner_{};
};
