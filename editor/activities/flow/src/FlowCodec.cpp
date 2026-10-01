#include <lux/engine/editor/flowforge/FlowCodec.hpp>

namespace lux::editor::flowforge
{
    using namespace persistence;
    PersistenceResult<EncodedArtifact> FlowCodec::encode(
        const FlowSnapshot& snapshot,
        asset::AssetId identity,
        std::stop_token stop
    )
    {
        if (stop.stop_requested())
            return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::CANCELLED});
        auto source = snapshot.source();
        source.id = identity;
        auto encoded = lux::flowforge::encodeFlowSource(source);
        if (!encoded)
            return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::ENCODE, encoded.error().field});
        auto bytes = std::as_bytes(std::span(*encoded));
        return EncodedArtifact{{bytes.begin(), bytes.end()}};
    }
    PersistenceResult<PreparedFlowData> FlowCodec::decode(std::span<const std::byte> bytes, std::stop_token stop)
    {
        if (stop.stop_requested())
            return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::CANCELLED});
        auto decoded = lux::flowforge::decodeFlowSource({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
        if (!decoded)
            return lux::cxx::unexpected(PersistenceFailure{EPersistenceError::DECODE, decoded.error().field});
        return PreparedFlowData{std::move(*decoded)};
    }
    FlowEditResult<std::unique_ptr<FlowSession>> PreparedFlowData::createSession(
        sessions::SessionId id,
        sessions::SourceBinding binding,
        lux::flowforge::FlowSourceEnvironment environment
    ) &&
    {
        auto graph = lux::flowforge::materializeFlowSource(source, environment);
        if (!graph)
        {
            FlowEditError error;
            error.source = std::move(graph.error());
            return lux::cxx::unexpected(std::move(error));
        }
        return FlowSession::create(
            id,
            std::move(binding),
            {source.id, std::move(source.name), std::move(*graph)},
            std::move(environment)
        );
    }
}
