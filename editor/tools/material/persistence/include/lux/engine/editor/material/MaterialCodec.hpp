#pragma once
#include <lux/engine/editor/material/MaterialSession.hpp>
#include <lux/engine/editor/persistence/EncodeJob.hpp>
namespace lux::editor::material
{
    struct PreparedMaterialData final
    {
        lux::material::MaterialSource source;
        [[nodiscard]] MaterialEditResult<std::unique_ptr<MaterialSession>> createSession(
            sessions::SessionId,
            sessions::SourceBinding,
            contracts::CodeLease code = contracts::CodeLease::builtin()
        ) &&;
    };
    class MaterialCodec final
    {
    public:
        [[nodiscard]] static persistence::PersistenceResult<persistence::EncodedArtifact> encode(
            const MaterialSnapshot&,
            asset::AssetId identity,
            std::stop_token = {}
        );
        [[nodiscard]] static persistence::PersistenceResult<PreparedMaterialData> decode(
            std::span<const std::byte>,
            std::stop_token = {}
        );
    };
}
