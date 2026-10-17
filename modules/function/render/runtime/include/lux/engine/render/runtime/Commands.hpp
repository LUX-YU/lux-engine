#pragma once

#include <optional>
#include <string>
#include <tuple>
#include <vector>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/render/data/Payload.hpp>
#include <lux/engine/render/data/Schema.hpp>
#include <lux/engine/render/protocol/Protocol.hpp>
#include <lux/engine/render/runtime/visibility.h>

namespace lux::render
{
    struct FeatureSelection final
    {
        // Static Schema fields/names and any foreign config deleter remain pinned through destruction.
        std::shared_ptr<const void> code_pin;
        std::string type;
        std::string instance;
        cxx::SchemaVersion version{1, 0};
        std::optional<DataSchemaDescriptor> config_schema;
        RenderPayload config;

        FeatureSelection(std::string type_name, std::string instance_name, cxx::SchemaVersion input_version = {1, 0},
            std::optional<DataSchemaDescriptor> schema = {}, RenderPayload input = {},
            std::shared_ptr<const void> pin = {}) noexcept
            : code_pin(std::move(pin)), type(std::move(type_name)), instance(std::move(instance_name)),
              version(input_version), config_schema(schema), config(std::move(input)) {}

        FeatureSelection(const FeatureSelection&) = default;
        FeatureSelection(FeatureSelection&&) noexcept = default;

        FeatureSelection& operator=(FeatureSelection other) noexcept
        {
            // Both module pins stay alive until every dependent config owner has been exchanged.
            using std::swap;
            swap(code_pin, other.code_pin);
            swap(type, other.type);
            swap(instance, other.instance);
            swap(version, other.version);
            swap(config_schema, other.config_schema);
            swap(config, other.config);
            return *this;
        }
    };

    struct EmptyRequest final {};
    struct SceneRequest final { SceneId scene{}; };
    struct CreateViewRequest final { SceneId scene{}; std::array<float, 4> color{}; };
    struct ViewRequest final { SceneId scene{}; ViewId view{}; };
    struct CreateTargetRequest final { TargetDesc description{}; };
    struct ResizeTargetRequest final { TargetId target{}; TargetDesc description{}; };
    struct EffectRequest final { EffectId effect{}; };
    struct AttachmentRequest final { SceneId scene{}; ViewId view{}; TargetId target{}; EffectId effect{}; };
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
        std::vector<FeatureSelection> features;
        std::vector<RenderPayload> attachments;
    };

    struct FeatureInfo final
    {
        FeatureId id;
        std::string name;
    };

    struct FeatureList final { std::vector<FeatureInfo> features; };

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
            }
            return bytes;
        }
    };

    struct CommandInfo final
    {
        CommandId id;
        std::string name;
        bool bound = false;
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
        using Response = Ack;
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
        using Request = AttachmentRequest;
        using Response = ReadbackResult;
    };

    struct Stop final
    {
        static constexpr std::string_view name{"render.stop"};
        using Request = EmptyRequest;
        using Response = Ack;
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
