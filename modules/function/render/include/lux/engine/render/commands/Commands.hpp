#pragma once

#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/commands/Frame.hpp>
#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/engine/render/core/Execution.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/core/RasterViewport.hpp>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/render/features/EffectDefinition.hpp>
#include <lux/engine/render/graph/Draw.hpp>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

namespace lux::render
{
    struct EmptyRequest final
    {
    };

    struct SceneRequest final
    {
        SceneId scene{};
    };

    struct CreateViewRequest final
    {
        SceneId scene{};
    };

    struct ViewRequest final
    {
        SceneId scene{};
        ViewId view{};
    };

    struct CreateTargetRequest final
    {
        TargetDesc description{};
        std::optional<SurfaceDescription> surface;
    };

    struct ResizeTargetRequest final
    {
        TargetId target{};
        TargetDesc description{};
    };

    struct EffectRequest final
    {
        EffectId effect{};
    };

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
    };

    struct AttachmentInputs final
    {
        AttachmentId attachment;
        std::vector<std::byte> parameters;
        RasterViewport viewport;
        std::vector<std::uint8_t> conditions;
    };

    struct GlobalStorageLocation final
    {
    };

    using VStorageLocation = std::variant<GlobalStorageLocation, SceneId, ViewRequest, AttachmentId>;

    struct StorageSelection final
    {
        VStorageLocation location;
        StorageTypeId type;
        std::string key;
    };

    struct PrepareStorageRequest final
    {
        StorageSelection location;
        ConfigValue config;
    };

    struct StorageBindingInfo final
    {
        std::string resource_key;
        StorageId storage;
    };

    struct AttachmentCreated final
    {
        RenderError error{};
        AttachmentId attachment;
        std::vector<StorageBindingInfo> storages;
        // Cold pass names/IDs for streaming geometry. IDs belong to this attachment's compiled graph.
        std::vector<std::pair<std::string, GraphPassId>> draw_lists;
    };

    struct DrawListUpdate final
    {
        AttachmentId attachment;
        GraphPassId pass;
        // Frozen through Response. Vertex format, index width and parameter layout come from the pass.
        DrawListData data;
    };

    struct UpdateDrawList final
    {
        static constexpr std::string_view name{"render.attachment.draw_list"};
        static constexpr bool frame_scoped = true;
        using Request = DrawListUpdate;
        using Response = AckResponse;
    };

    struct FindStorage final
    {
        static constexpr std::string_view name{"render.storage.find"};
        static constexpr bool frame_scoped = false;
        using Request = StorageSelection;

        struct Response final
        {
            RenderError error{};
            StorageId storage{};
        };
    };

    struct PrepareStorage final
    {
        static constexpr std::string_view name{"render.storage.prepare"};
        static constexpr bool frame_scoped = false;
        using Request = PrepareStorageRequest;

        struct Response final
        {
            RenderError error{};
            StorageId storage{};
        };
    };

    struct RemoveStorage final
    {
        static constexpr std::string_view name{"render.storage.remove"};
        static constexpr bool frame_scoped = true;
        using Request = StorageId;
        using Response = AckResponse;
    };

    struct SceneCreated final
    {
        RenderError error{};
        SceneId scene{};
    };

    struct ViewCreated final
    {
        RenderError error{};
        ViewId view{};
    };

    struct TargetCreated final
    {
        RenderError error{};
        TargetId target{};
        std::uint64_t epoch = 0;
        TargetDesc description; // Actual output format/extent selected by the backend.
    };

    struct EffectCreated final
    {
        RenderError error{};
        EffectId effect{};
    };

    struct RenderStatsSnapshot final
    {
        RenderError error{};
        std::uint64_t ticks = 0, commands = 0, compiled_graphs = 0, submissions = 0;
    };

    struct CreateEffectRequest final
    {
        TargetDesc description;
        EffectDefinition definition;
    };

    struct FeatureInfo final
    {
        FeatureId id;
        std::string name;
        std::string config_type;
        std::string description;
    };

    struct FeatureList final
    {
        RenderError error{};
        std::vector<FeatureInfo> features;
    };

    struct StorageInfo final
    {
        StorageTypeId id;
        std::string name;
        std::uint32_t capabilities = 0;
        std::string description;
    };

    struct StorageList final
    {
        RenderError error{};
        std::vector<StorageInfo> storages;
    };

    struct ListStorages final
    {
        static constexpr std::string_view name{"render.storages.list"};
        static constexpr bool frame_scoped = false;
        using Request = EmptyRequest;
        using Response = StorageList;
    };

    struct ListFeatures final
    {
        static constexpr std::string_view name{"render.features.list"};
        static constexpr bool frame_scoped = false;
        using Request = EmptyRequest;
        using Response = FeatureList;
    };

    struct CommandInfo final
    {
        // Names are owned; type names borrow the defining module, which must outlive clients and the Server.
        std::string name;
        CommandId id;
        cxx::TypeToken request;
        cxx::TypeToken response;
        bool bound = false;
        bool frame_scoped = false;

        [[nodiscard]] CommandDescriptor descriptor() const noexcept
        {
            return {CommandName{name}, request, response, frame_scoped};
        }
    };

    struct CommandList final
    {
        RenderError error{};
        std::vector<CommandInfo> commands;
    };

    struct HandlerInfo final
    {
        HandlerId id;
        std::string name;
        std::string instance;
    };

    struct CommandBindings final
    {
        RenderError error{};
        HandlerId selected;
        std::vector<HandlerInfo> handlers;
    };

    struct CommandSelection final
    {
        CommandId command;
        HandlerId handler;
    };

    struct ListCommands final
    {
        static constexpr std::string_view name{"render.commands.list"};
        static constexpr bool frame_scoped = false;
        using Request = EmptyRequest;
        using Response = CommandList;
    };

    struct ListCommandBindings final
    {
        static constexpr std::string_view name{"render.commands.bindings"};
        static constexpr bool frame_scoped = false;
        using Request = CommandId;
        using Response = CommandBindings;
    };

    struct SelectCommandBinding final
    {
        static constexpr std::string_view name{"render.commands.select"};
        static constexpr bool frame_scoped = false;
        using Request = CommandSelection;
        using Response = AckResponse;
    };

    struct CreateScene final
    {
        static constexpr std::string_view name{"render.scene.create"};
        static constexpr bool frame_scoped = false;
        using Request = EmptyRequest;
        using Response = SceneCreated;
    };

    struct RemoveScene final
    {
        static constexpr std::string_view name{"render.scene.remove"};
        static constexpr bool frame_scoped = true;
        using Request = SceneRequest;
        using Response = AckResponse;
    };

    struct CreateView final
    {
        static constexpr std::string_view name{"render.view.create"};
        static constexpr bool frame_scoped = false;
        using Request = CreateViewRequest;
        using Response = ViewCreated;
    };

    struct UpdateAttachmentInputs final
    {
        static constexpr std::string_view name{"render.attachment.inputs"};
        static constexpr bool frame_scoped = true;
        using Request = AttachmentInputs;
        using Response = AckResponse;
    };

    struct RemoveView final
    {
        static constexpr std::string_view name{"render.view.remove"};
        static constexpr bool frame_scoped = true;
        using Request = ViewRequest;
        using Response = AckResponse;
    };

    struct InvalidateHistory final
    {
        static constexpr std::string_view name{"render.attachment.invalidate_history"};
        static constexpr bool frame_scoped = true;

        struct Request final
        {
            AttachmentId attachment;
            std::vector<GraphResourceId> previous;
        };

        using Response = AckResponse;
    };

    struct CreateTarget final
    {
        static constexpr std::string_view name{"render.target.create"};
        static constexpr bool frame_scoped = false;
        using Request = CreateTargetRequest;
        using Response = TargetCreated;
    };

    struct ResizeTarget final
    {
        static constexpr std::string_view name{"render.target.resize"};
        static constexpr bool frame_scoped = true;
        using Request = ResizeTargetRequest;
        using Response = TargetCreated;
    };

    struct RemoveTarget final
    {
        static constexpr std::string_view name = "render.target.remove";
        static constexpr bool frame_scoped = true;
        using Request = TargetId;
        using Response = AckResponse;
    };

    struct SetTargetActive final
    {
        static constexpr std::string_view name = "render.target.set_active";
        static constexpr bool frame_scoped = true;

        struct Request final
        {
            TargetId target;
            bool active = true;
        };

        using Response = AckResponse;
    };

    struct CreateEffect final
    {
        static constexpr std::string_view name{"render.effect.create"};
        static constexpr bool frame_scoped = false;
        using Request = CreateEffectRequest;
        using Response = EffectCreated;
    };

    struct RemoveEffect final
    {
        static constexpr std::string_view name{"render.effect.remove"};
        static constexpr bool frame_scoped = true;
        using Request = EffectRequest;
        using Response = AckResponse;
    };

    struct Attach final
    {
        static constexpr std::string_view name{"render.attach"};
        static constexpr bool frame_scoped = true;
        using Request = AttachmentRequest;
        using Response = AttachmentCreated;
    };

    struct Detach final
    {
        static constexpr std::string_view name{"render.detach"};
        static constexpr bool frame_scoped = true;
        using Request = AttachmentId;
        using Response = AckResponse;
    };

    struct Stop final
    {
        static constexpr std::string_view name{"render.stop"};
        static constexpr bool frame_scoped = false;
        using Request = EmptyRequest;
        using Response = AckResponse;
    };

    struct QueryExecutionProgress final
    {
        static constexpr std::string_view name{"render.execution.progress"};
        static constexpr bool frame_scoped = false;
        using Request = AttachmentId;

        struct Response final
        {
            RenderError error{};
            ExecutionProgress progress{};
        };
    };

    struct QueryRenderStats final
    {
        static constexpr std::string_view name{"render.stats.query"};
        static constexpr bool frame_scoped = false;
        using Request = EmptyRequest;
        using Response = RenderStatsSnapshot;
    };

} // namespace lux::render
