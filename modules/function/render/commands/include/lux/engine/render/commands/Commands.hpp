#pragma once

#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/core/Execution.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/core/RasterViewport.hpp>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/render/data/ConfigValue.hpp>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

namespace lux::render
{
    struct FeatureUse final
    {
        FeatureId feature;
        std::string instance;
        ConfigValue config;
    };

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

    struct AttachmentRequest final
    {
        SceneId scene;
        ViewId view;
        TargetId target;
        EffectId effect;
        std::vector<std::byte> parameters; // Exact bytes of the compiled context parameter block.
        RasterViewport viewport;
    };

    struct AttachmentInputs final
    {
        AttachmentId attachment;
        std::vector<std::byte> parameters;
        RasterViewport viewport;
    };

    struct GlobalStorageOwner final
    {
    };

    using VStorageOwner = std::variant<GlobalStorageOwner, SceneId, ViewRequest, AttachmentId>;

    struct StorageLocation final
    {
        VStorageOwner owner;
        StorageTypeId type;
        std::string key;
    };

    struct PrepareStorageRequest final
    {
        StorageLocation location;
        ConfigValue config;
    };

    struct StorageBindingInfo final
    {
        std::string resource_key;
        StorageId storage;
    };

    struct AttachmentCreated final
    {
        AttachmentId attachment;
        std::vector<StorageBindingInfo> storages;
    };

    struct FindStorage final
    {
        static constexpr std::string_view name{"render.storage.find"};
        using Request = StorageLocation;
        using Response = RenderResult<StorageId>;
    };

    struct PrepareStorage final
    {
        static constexpr std::string_view name{"render.storage.prepare"};
        using Request = PrepareStorageRequest;
        using Response = RenderResult<StorageId>;
    };

    struct RemoveStorage final
    {
        static constexpr std::string_view name{"render.storage.remove"};
        using Request = StorageId;
        using Response = RenderResult<Ack>;
    };

    struct SceneCreated final
    {
        SceneId scene{};
    };

    struct ViewCreated final
    {
        ViewId view{};
    };

    struct TargetCreated final
    {
        TargetId target{};
        std::uint64_t epoch = 0;
    };

    struct EffectCreated final
    {
        EffectId effect{};
    };

    struct RenderStatsSnapshot final
    {
        std::uint64_t ticks = 0, commands = 0, compiled_graphs = 0, submissions = 0;
    };

    struct CreateEffectRequest final
    {
        TargetDesc description;
        std::vector<FeatureUse> features;
    };

    struct FeatureInfo final
    {
        FeatureId id;
        std::string name;
        std::string config_type;
        std::size_t config_size = 0, config_alignment = 0;
    };

    struct FeatureList final
    {
        std::vector<FeatureInfo> features;
    };

    struct StorageInfo final
    {
        StorageTypeId id;
        std::string name;
        std::uint32_t capabilities = 0;
    };

    struct StorageList final
    {
        std::vector<StorageInfo> storages;
    };

    struct ListStorages final
    {
        static constexpr std::string_view name{"render.storages.list"};
        using Request = EmptyRequest;
        using Response = RenderResult<StorageList>;
    };

    struct ListFeatures final
    {
        static constexpr std::string_view name{"render.features.list"};
        using Request = EmptyRequest;
        using Response = RenderResult<FeatureList>;
    };

    struct CommandInfo final
    {
        // Cold directory values retain the provider of the canonical value operations.
        std::string name;
        CommandId id;
        CppType request;
        CppType response;
        bool bound = false;

        [[nodiscard]] CommandDescriptor descriptor() const noexcept
        {
            return {CommandName{name}, request, response};
        }
    };

    struct CommandList final
    {
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
        using Request = EmptyRequest;
        using Response = RenderResult<CommandList>;
    };

    struct ListCommandBindings final
    {
        static constexpr std::string_view name{"render.commands.bindings"};
        using Request = CommandId;
        using Response = RenderResult<CommandBindings>;
    };

    struct SelectCommandBinding final
    {
        static constexpr std::string_view name{"render.commands.select"};
        using Request = CommandSelection;
        using Response = RenderResult<Ack>;
    };

    struct CreateScene final
    {
        static constexpr std::string_view name{"render.scene.create"};
        using Request = EmptyRequest;
        using Response = RenderResult<SceneCreated>;
    };

    struct RemoveScene final
    {
        static constexpr std::string_view name{"render.scene.remove"};
        using Request = SceneRequest;
        using Response = RenderResult<Ack>;
    };

    struct CreateView final
    {
        static constexpr std::string_view name{"render.view.create"};
        using Request = CreateViewRequest;
        using Response = RenderResult<ViewCreated>;
    };

    struct UpdateAttachmentInputs final
    {
        static constexpr std::string_view name{"render.attachment.inputs"};
        using Request = AttachmentInputs;
        using Response = RenderResult<Ack>;
    };

    struct RemoveView final
    {
        static constexpr std::string_view name{"render.view.remove"};
        using Request = ViewRequest;
        using Response = RenderResult<Ack>;
    };

    struct CreateTarget final
    {
        static constexpr std::string_view name{"render.target.create"};
        using Request = CreateTargetRequest;
        using Response = RenderResult<TargetCreated>;
    };

    struct ResizeTarget final
    {
        static constexpr std::string_view name{"render.target.resize"};
        using Request = ResizeTargetRequest;
        using Response = RenderResult<TargetCreated>;
    };

    struct CreateEffect final
    {
        static constexpr std::string_view name{"render.effect.create"};
        using Request = CreateEffectRequest;
        using Response = RenderResult<EffectCreated>;
    };

    struct RemoveEffect final
    {
        static constexpr std::string_view name{"render.effect.remove"};
        using Request = EffectRequest;
        using Response = RenderResult<Ack>;
    };

    struct Attach final
    {
        static constexpr std::string_view name{"render.attach"};
        using Request = AttachmentRequest;
        using Response = RenderResult<AttachmentCreated>;
    };

    struct Detach final
    {
        static constexpr std::string_view name{"render.detach"};
        using Request = AttachmentId;
        using Response = RenderResult<Ack>;
    };

    struct Stop final
    {
        static constexpr std::string_view name{"render.stop"};
        using Request = EmptyRequest;
        using Response = RenderResult<Ack>;
    };

    struct QueryExecutionProgress final
    {
        static constexpr std::string_view name{"render.execution.progress"};
        using Request = AttachmentId;
        using Response = RenderResult<ExecutionProgress>;
    };

    struct QueryRenderStats final
    {
        static constexpr std::string_view name{"render.stats.query"};
        using Request = EmptyRequest;
        using Response = RenderResult<RenderStatsSnapshot>;
    };

} // namespace lux::render
