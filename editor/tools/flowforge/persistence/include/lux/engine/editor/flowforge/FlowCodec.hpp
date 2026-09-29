#pragma once
#include <lux/engine/editor/flowforge/FlowSession.hpp>
#include <lux/engine/editor/persistence/EncodeJob.hpp>
namespace lux::editor::flowforge
{
    struct PreparedFlowData final
    {
        lux::flowforge::FlowSource source;
        [[nodiscard]] FlowEditResult<std::unique_ptr<FlowSession>> createSession(
            sessions::SessionId,
            sessions::SourceBinding,
            lux::flowforge::FlowSourceEnvironment environment = {}
        ) &&;
    };
    class FlowCodec final
    {
    public:
        [[nodiscard]] static persistence::PersistenceResult<persistence::EncodedArtifact> encode(
            const FlowSnapshot&,
            asset::AssetId identity,
            std::stop_token = {}
        );
        [[nodiscard]] static persistence::PersistenceResult<PreparedFlowData> decode(
            std::span<const std::byte>,
            std::stop_token = {}
        );
    };
}
