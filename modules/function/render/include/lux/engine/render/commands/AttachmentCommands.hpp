#pragma once

#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/core/Execution.hpp>
#include <lux/engine/render/core/RasterViewport.hpp>
#include <vector>

namespace lux::render
{
    // A sampled offscreen Target supplies this declared imported image in the same sealed frame.
    // The producer must remain attached and enabled. Detach consumers before removing its Target.
    struct TargetImageInput final
    {
        std::string resource_key;
        TargetId target;
    };

    struct AttachmentRequest final
    {
        SceneId scene;
        ViewId view;
        TargetId target;
        EffectId effect;
        std::vector<std::byte> parameters; // Exact bytes of the compiled context parameter block.
        RasterViewport viewport;
        std::vector<std::uint8_t> conditions;
        // Optional current attachment of the same Scene/View. The destination Target must be free
        // or contain that attachment. Prepare the complete candidate before atomically replacing
        // the old writer; this also supports moving a view to another output without a detach gap.
        AttachmentId replaces;
        std::vector<TargetImageInput> target_images;
        cxx::type_info parameter_type; // Public semantic input type, projected by the selected capability.
    };

    struct AttachmentInputs final
    {
        AttachmentId attachment;
        std::vector<std::byte> parameters;
        RasterViewport viewport;
        std::vector<std::uint8_t> conditions;
        cxx::type_info parameter_type;
    };

    struct AttachmentCreated final
    {
        RenderError error{};
        AttachmentId attachment;
    };

    struct UpdateAttachmentInputs final
    {
        static constexpr std::string_view name{"render.attachment.inputs"};
        static constexpr bool requires_frame_boundary = true;
        using Request = AttachmentInputs;
        using Response = AckResponse;
    };

    struct InvalidateHistory final
    {
        static constexpr std::string_view name{"render.attachment.invalidate_history"};
        static constexpr bool requires_frame_boundary = true;

        struct Request final
        {
            AttachmentId attachment;
            std::vector<GraphResourceId> previous;
        };

        using Response = AckResponse;
    };

    struct Attach final
    {
        static constexpr std::string_view name{"render.attach"};
        static constexpr bool requires_frame_boundary = true;
        using Request = AttachmentRequest;
        using Response = AttachmentCreated;
    };

    struct Detach final
    {
        static constexpr bool failure_safe = true;
        static constexpr std::string_view name{"render.detach"};
        static constexpr bool requires_frame_boundary = true;
        using Request = AttachmentId;
        using Response = AckResponse;
    };

    struct QueryExecutionProgress final
    {
        static constexpr bool failure_safe = true;
        static constexpr std::string_view name{"render.execution.progress"};
        static constexpr bool requires_frame_boundary = false;
        using Request = AttachmentId;

        struct Response final
        {
            RenderError error{};
            ExecutionProgress progress{};
        };
    };
} // namespace lux::render
