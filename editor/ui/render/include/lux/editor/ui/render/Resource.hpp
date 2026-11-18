#pragma once

#include <lux/editor/ui/render/DrawData.hpp>
#include <lux/engine/render/capabilities/CapabilityDeclaration.hpp>
#include <lux/engine/render/extensions/resource/ResourceDefinition.hpp>
#include <lux/engine/render/resources/texture/Resource.hpp>

namespace lux::editor::ui
{
    struct SubmitDrawData final
    {
        static constexpr std::string_view name = "lux.editor.ui.submit_draw_data";
        static constexpr bool requires_idle_resource = true;
        using Data = ImguiFrameData;

        struct Request final
        {
            render::ResourceInstanceId resource_id;
            ImguiFrameData data;
        };

        struct Response final
        {
            render::RenderError error{};
        };

        static render::ResourceInstanceId resourceId(const Request& request) noexcept
        {
            return request.resource_id;
        }
    };

    struct UiBinding final
    {
        bool operator==(const UiBinding&) const noexcept = default;
    };

    struct UiResource final
    {
        using Commands = std::tuple<SubmitDrawData>;
        using PerFrameData = std::tuple<>;
        using Config = void;
        using Ops = void;
        static inline const render::ResourceDefinition descriptor{
            render::ResourceNameView{"lux.editor.ui.draw_data"},
            0,
            0,
            cxx::make_basic_type_info<UiBinding>(),
            "Owned complete UI drawing snapshot; empty replaces empty.",
            render::EResourceDomain::VIEW,
            render::resourceCommandDescriptors(static_cast<Commands*>(nullptr))
        };
    };

    struct UiCapability final
    {
        using PrerequisiteCapabilities = std::tuple<>;
        using Commands = std::tuple<>;
        using Resources = std::tuple<render::TextureResource, UiResource>;
    };
} // namespace lux::editor::ui
