#pragma once

#include <lux/engine/object/CodeLease.hpp>
#include <lux/engine/editor/persistence/WriteLane.hpp>
#include <lux/engine/resource/identity/AssetId.hpp>
#include <lux/engine/resource/asset/AssetTypeId.hpp>
#include <stop_token>

namespace lux::editor::persistence
{
    // Frozen domain source encoding. Implementations retain their immutable capture, never a live Session.
    class IArtifactSource
    {
    public:
        virtual ~IArtifactSource() = default;
        [[nodiscard]] virtual PersistenceResult<EncodedArtifact> encode(std::stop_token) const = 0;
    };
    struct DerivedArtifactInfo final
    {
        sessions::ContentStamp content;
        asset::AssetId source_asset;
        std::string canonical_type;
        std::uint32_t encoding_version{1};
        std::uint32_t primary_magic{};
        [[nodiscard]] asset::AssetTypeId type() const noexcept
        {
            return asset::AssetTypeId::fromName(canonical_type);
        }
    };
    // Copying shares frozen bytes/source, not task control. Code outlives both payload and control blocks.
    class DerivedArtifact final
    {
    public:
        DerivedArtifact(
            lux::object::CodeLease, DerivedArtifactInfo, cxx::SharedBytes<>, std::shared_ptr<const IArtifactSource>
        );
        DerivedArtifact(const DerivedArtifact&);
        DerivedArtifact(DerivedArtifact&&) noexcept = default;
        DerivedArtifact& operator=(DerivedArtifact) noexcept;
        ~DerivedArtifact() = default;
        [[nodiscard]] bool valid() const noexcept;
        [[nodiscard]] const DerivedArtifactInfo& info() const noexcept { return info_; }
        [[nodiscard]] const cxx::SharedBytes<>& bytes() const noexcept { return bytes_; }
        [[nodiscard]] PersistenceResult<EncodedArtifact> encodeSource(std::stop_token) const noexcept;

    private:
        lux::object::CodeLease code_;
        DerivedArtifactInfo info_;
        cxx::SharedBytes<> bytes_;
        std::shared_ptr<const IArtifactSource> source_;
    };
}
