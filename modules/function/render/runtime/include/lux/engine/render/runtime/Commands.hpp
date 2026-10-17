#pragma once

#include <optional>
#include <memory>
#include <string>
#include <tuple>
#include <variant>
#include <vector>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/render/core/Camera.hpp>
#include <lux/engine/render/core/Execution.hpp>
#include <lux/engine/render/data/Payload.hpp>
#include <lux/engine/render/data/ConfigValue.hpp>
#include <lux/engine/render/protocol/Protocol.hpp>
#include <lux/engine/render/runtime/visibility.h>

namespace lux::platform::ipc { class IpcTypeOps; }

namespace lux::render
{
    struct FeatureUse final
    {
        FeatureId feature;
        std::string instance;
        ConfigValue config;
    };

    struct EmptyRequest final {};
    struct SceneRequest final { SceneId scene{}; };
    struct CreateViewRequest final { SceneId scene{}; CameraState camera{}; };
    struct UpdateCameraRequest final { SceneId scene{}; ViewId view{}; CameraState camera{}; };
    struct ViewRequest final { SceneId scene{}; ViewId view{}; };
    struct CreateTargetRequest final { TargetDesc description{}; };
    struct ResizeTargetRequest final { TargetId target{}; TargetDesc description{}; };
    struct EffectRequest final { EffectId effect{}; };
    struct AttachmentRequest final { SceneId scene{}; ViewId view{}; TargetId target{}; EffectId effect{}; };
    struct ReadbackRequest final
    {
        AttachmentRequest attachment;
        std::string output; // Empty selects the explicit Target copy; otherwise an exported buffer key.
    };

    template <> struct CommandValueTraits<ReadbackRequest> final
    {
        [[nodiscard]] static RenderResult<std::size_t> retainedBytes(const ReadbackRequest& value) noexcept
        {
            if (value.output.capacity() >= SIZE_MAX - sizeof(value))
                return cxx::unexpected(RenderError{kProtocolContract});
            return sizeof(value) + value.output.capacity() + 1;
        }
    };

    struct GlobalStorageOwner final {};
    using VStorageOwner = std::variant<GlobalStorageOwner, SceneId, ViewRequest, AttachmentRequest>;
    struct StorageLocation final
    {
        VStorageOwner owner;
        StorageTypeId type;
        std::string key;
    };
    struct PrepareStorageRequest final { StorageLocation location; ConfigValue config; };
    struct StorageBindingInfo final { std::string resource_key; StorageId storage; };
    struct AttachmentCreated final { std::vector<StorageBindingInfo> storages; };

    struct FindStorage final
    {
        static constexpr std::string_view name{"render.storage.find"};
        using Request = StorageLocation;
        using Response = StorageId;
    };
    struct PrepareStorage final
    {
        static constexpr std::string_view name{"render.storage.prepare"};
        using Request = PrepareStorageRequest;
        using Response = StorageId;
    };
    struct ReleasePreparedStorage final
    {
        static constexpr std::string_view name{"render.storage.release_prepared"};
        using Request = StorageId;
        using Response = Ack;
    };

    template <> struct CommandValueTraits<StorageLocation>
    {
        [[nodiscard]] static RenderResult<std::size_t> retainedBytes(const StorageLocation& value) noexcept
        {
            if (value.key.capacity() >= SIZE_MAX - sizeof(value))
                return cxx::unexpected(RenderError{kProtocolContract});
            return sizeof(value) + value.key.capacity() + 1;
        }
    };

    template <> struct CommandValueTraits<PrepareStorageRequest>
    {
        [[nodiscard]] static RenderResult<std::size_t> retainedBytes(const PrepareStorageRequest& value) noexcept
        {
            auto bytes = CommandValueTraits<StorageLocation>::retainedBytes(value.location);
            if (!bytes) return cxx::unexpected(bytes.error());
            if (*bytes > SIZE_MAX - sizeof(ConfigValue)) return cxx::unexpected(RenderError{kProtocolContract});
            *bytes += sizeof(ConfigValue);
            if (value.config.retainedBytes() > SIZE_MAX - *bytes)
                return cxx::unexpected(RenderError{kProtocolContract});
            return *bytes + value.config.retainedBytes();
        }
    };

    template <> struct CommandValueTraits<AttachmentCreated>
    {
        [[nodiscard]] static RenderResult<std::size_t> retainedBytes(const AttachmentCreated& value) noexcept
        {
            if (value.storages.capacity() > (SIZE_MAX - sizeof(value)) / sizeof(StorageBindingInfo))
                return cxx::unexpected(RenderError{kProtocolContract});
            auto bytes = sizeof(value) + value.storages.capacity() * sizeof(StorageBindingInfo);
            for (const auto& item : value.storages)
            {
                if (item.resource_key.capacity() >= SIZE_MAX - bytes)
                    return cxx::unexpected(RenderError{kProtocolContract});
                bytes += item.resource_key.capacity() + 1;
            }
            return bytes;
        }
    };

    struct SceneCreated final { SceneId scene{}; };
    struct ViewCreated final { ViewId view{}; };
    struct TargetCreated final { TargetId target{}; std::uint64_t epoch = 0; };
    struct EffectCreated final { EffectId effect{}; };
    struct ReadbackResult final { RenderPayload pixels; };
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

    struct FeatureList final { std::vector<FeatureInfo> features; };

    struct StorageInfo final
    {
        StorageTypeId id;
        std::string name;
        std::uint32_t capabilities = 0;
    };

    struct StorageList final { std::vector<StorageInfo> storages; };

    struct ListStorages final
    {
        static constexpr std::string_view name{"render.storages.list"};
        using Request = EmptyRequest;
        using Response = StorageList;
    };

    template <> struct CommandValueTraits<StorageList>
    {
        [[nodiscard]] static RenderResult<std::size_t> retainedBytes(const StorageList& value) noexcept
        {
            std::size_t bytes = sizeof(value);
            if (value.storages.capacity() > (SIZE_MAX - bytes) / sizeof(StorageInfo))
                return cxx::unexpected(RenderError{kProtocolContract});
            bytes += value.storages.capacity() * sizeof(StorageInfo);
            for (const auto& storage : value.storages)
            {
                if (storage.name.capacity() >= SIZE_MAX - bytes)
                    return cxx::unexpected(RenderError{kProtocolContract});
                bytes += storage.name.capacity() + 1;
            }
            return bytes;
        }
    };

    struct ListFeatures final
    {
        static constexpr std::string_view name{"render.features.list"};
        using Request = EmptyRequest;
        using Response = FeatureList;
    };

    template <> struct CommandValueTraits<FeatureList>
    {
        [[nodiscard]] static RenderResult<std::size_t> retainedBytes(const FeatureList& value) noexcept
        {
            std::size_t bytes = sizeof(value);
            if (value.features.capacity() > (SIZE_MAX - bytes) / sizeof(FeatureInfo))
                return cxx::unexpected(RenderError{kProtocolContract});
            bytes += value.features.capacity() * sizeof(FeatureInfo);
            for (const auto& feature : value.features)
            {
                if (feature.name.capacity() >= SIZE_MAX - bytes)
                    return cxx::unexpected(RenderError{kProtocolContract});
                bytes += feature.name.capacity() + 1;
                if (feature.config_type.capacity() >= SIZE_MAX - bytes)
                    return cxx::unexpected(RenderError{kProtocolContract});
                bytes += feature.config_type.capacity() + 1;
            }
            return bytes;
        }
    };

    struct CommandInfo final
    {
        // Cold directory values retain the provider of the canonical value operations.
        std::shared_ptr<const void> code_pin;
        std::string name;
        CommandId id;
        CommandValueType request;
        CommandValueType response;
        const platform::ipc::IpcTypeOps* request_ops = nullptr;
        const platform::ipc::IpcTypeOps* response_ops = nullptr;
        bool bound = false;

        [[nodiscard]] CommandDescriptor descriptor() const noexcept
        { return {CommandName{name}, request, response}; }
    };
    struct CommandList final { std::vector<CommandInfo> commands; };
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
    struct CommandSelection final { CommandId command; HandlerId handler; };
    struct ListCommands final
    {
        static constexpr std::string_view name{"render.commands.list"};
        using Request = EmptyRequest;
        using Response = CommandList;
    };
    struct ListCommandBindings final
    {
        static constexpr std::string_view name{"render.commands.bindings"};
        using Request = CommandId;
        using Response = CommandBindings;
    };
    struct SelectCommandBinding final
    {
        static constexpr std::string_view name{"render.commands.select"};
        using Request = CommandSelection;
        using Response = Ack;
    };
    template <> struct CommandValueTraits<CommandList>
    {
        [[nodiscard]] static RenderResult<std::size_t> retainedBytes(const CommandList& value) noexcept
        {
            std::size_t bytes = sizeof(value);
            if (value.commands.capacity() > (SIZE_MAX - bytes) / sizeof(CommandInfo))
                return cxx::unexpected(RenderError{kProtocolContract});
            bytes += value.commands.capacity() * sizeof(CommandInfo);
            for (const auto& command : value.commands)
            {
                if (command.name.capacity() >= SIZE_MAX - bytes)
                    return cxx::unexpected(RenderError{kProtocolContract});
                bytes += command.name.capacity() + 1;
            }
            return bytes;
        }
    };
    template <> struct CommandValueTraits<CommandBindings>
    {
        [[nodiscard]] static RenderResult<std::size_t> retainedBytes(const CommandBindings& value) noexcept
        {
            std::size_t bytes = sizeof(value);
            if (value.handlers.capacity() > (SIZE_MAX - bytes) / sizeof(HandlerInfo))
                return cxx::unexpected(RenderError{kProtocolContract});
            bytes += value.handlers.capacity() * sizeof(HandlerInfo);
            for (const auto& handler : value.handlers)
            {
                if (handler.name.capacity() >= SIZE_MAX - bytes)
                    return cxx::unexpected(RenderError{kProtocolContract});
                bytes += handler.name.capacity() + 1;
                if (handler.instance.capacity() >= SIZE_MAX - bytes)
                    return cxx::unexpected(RenderError{kProtocolContract});
                bytes += handler.instance.capacity() + 1;
            }
            return bytes;
        }
    };

    struct CreateScene final
    {
        static constexpr std::string_view name{"render.scene.create"};
        using Request = EmptyRequest;
        using Response = SceneCreated;
    };

    struct RemoveScene final
    {
        static constexpr std::string_view name{"render.scene.remove"};
        using Request = SceneRequest;
        using Response = Ack;
    };

    struct CreateView final
    {
        static constexpr std::string_view name{"render.view.create"};
        using Request = CreateViewRequest;
        using Response = ViewCreated;
    };

    struct UpdateCamera final
    {
        static constexpr std::string_view name{"render.view.camera"};
        using Request = UpdateCameraRequest;
        using Response = Ack;
    };

    struct RemoveView final
    {
        static constexpr std::string_view name{"render.view.remove"};
        using Request = ViewRequest;
        using Response = Ack;
    };

    struct CreateTarget final
    {
        static constexpr std::string_view name{"render.target.create"};
        using Request = CreateTargetRequest;
        using Response = TargetCreated;
    };

    struct ResizeTarget final
    {
        static constexpr std::string_view name{"render.target.resize"};
        using Request = ResizeTargetRequest;
        using Response = TargetCreated;
    };

    struct CreateEffect final
    {
        static constexpr std::string_view name{"render.effect.create"};
        using Request = CreateEffectRequest;
        using Response = EffectCreated;
    };

    struct RemoveEffect final
    {
        static constexpr std::string_view name{"render.effect.remove"};
        using Request = EffectRequest;
        using Response = Ack;
    };

    struct Attach final
    {
        static constexpr std::string_view name{"render.attach"};
        using Request = AttachmentRequest;
        using Response = AttachmentCreated;
    };

    struct Detach final
    {
        static constexpr std::string_view name{"render.detach"};
        using Request = AttachmentRequest;
        using Response = Ack;
    };

    struct Readback final
    {
        static constexpr std::string_view name{"render.readback"};
        using Request = ReadbackRequest;
        using Response = ReadbackResult;
    };

    struct Stop final
    {
        static constexpr std::string_view name{"render.stop"};
        using Request = EmptyRequest;
        using Response = Ack;
    };

    struct QueryExecutionProgress final
    {
        static constexpr std::string_view name{"render.execution.progress"};
        using Request = AttachmentRequest;
        using Response = ExecutionProgress;
    };

    struct QueryRenderStats final
    {
        static constexpr std::string_view name{"render.stats.query"};
        using Request = EmptyRequest;
        using Response = RenderStatsSnapshot;
    };

    template <> struct CommandValueTraits<CreateEffectRequest>
    {
        [[nodiscard]] static LUX_RENDER_CLIENT_PUBLIC RenderResult<std::size_t>
        retainedBytes(const CreateEffectRequest& value) noexcept;
    };

    template <> struct CommandValueTraits<ReadbackResult>
    {
        [[nodiscard]] static RenderResult<std::size_t> retainedBytes(const ReadbackResult& value) noexcept
        {
            if (value.pixels.retainedBytes() > SIZE_MAX - sizeof(value))
                return cxx::unexpected(RenderError{kProtocolContract});
            return sizeof(value) + value.pixels.retainedBytes();
        }
    };
}
