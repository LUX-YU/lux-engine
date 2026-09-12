#pragma once
#include <lux/engine/editor/ui/properties/PropertyDisplay.hpp>
#include <lux/engine/description/Visual.hpp>
#include <lux/engine/resource/identity/AssetId.hpp>
#include <lux/engine/editor/sessions/scene/SceneSession.hpp>
namespace lux::editor::ui
{
    inline void drawSceneReadOnlyField(sessions::SceneSession &session, lux::ui::Frame &frame,
                                      std::string_view label, const lux::asset::AssetId &id)
    {
        frame.propertyRow(label);
        if (id.isNull())
        {
            frame.text("<none>");
            return;
        }
        const auto path = session.readAssetPath(id);
        if (!path)
            frame.textMuted("Asset path is unavailable");
        else
            frame.text(*path ? **path : "<unresolved asset>");
    }
    template<class T> void drawSceneReadOnlyField(sessions::SceneSession &, lux::ui::Frame &frame,
                                                 std::string_view label, const T &value)
    {
        drawReadOnlyField(frame, label, value);
    }
    inline void drawSceneReadOnlyField(sessions::SceneSession &session, lux::ui::Frame &frame, std::string_view label,
                                      const rdesc::MeshVisualDescription &value)
    {
        frame.propertyRow(label);
        frame.text("Mesh visual");
        drawSceneReadOnlyField(session, frame, "Mesh", value.mesh);
        drawSceneReadOnlyField(session, frame, "Material", value.material);
        drawReadOnlyField(frame, "Visible", value.visible);
        drawReadOnlyField(frame, "Cast shadow", value.cast_shadow);
        drawReadOnlyField(frame, "Receive shadow", value.receive_shadow);
    }

    inline void drawSceneReadOnlyField(sessions::SceneSession &, lux::ui::Frame &frame,
                                      std::string_view label, const rdesc::LightDescription &value)
    {
        frame.propertyRow(label);
        frame.text("Light");
        drawReadOnlyField(frame, "Type", value.type);
        drawReadOnlyField(frame, "Red", value.color[0]);
        drawReadOnlyField(frame, "Green", value.color[1]);
        drawReadOnlyField(frame, "Blue", value.color[2]);
        drawReadOnlyField(frame, "Intensity", value.intensity);
        drawReadOnlyField(frame, "Range", value.range);
        drawReadOnlyField(frame, "Cast shadow", value.cast_shadow);
    }
} // namespace lux::editor::ui
