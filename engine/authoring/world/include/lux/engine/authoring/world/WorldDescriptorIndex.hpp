#pragma once

#include <lux/engine/authoring/world/visibility.h>

#include <lux/engine/authoring/world/WorldSource.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lux::authoring
{
    inline constexpr std::uint32_t kWorldDescriptorIndexMagic = 0x4944584cu;
    inline constexpr std::uint32_t kWorldDescriptorIndexVersion = 5u;

    struct WorldDescriptorIndexStats final
    {
        std::size_t actor_count{0u};
        std::size_t page_count{0u};
        std::size_t cached_actor_objects{0u};
        std::size_t cached_search_objects{0u};
        std::size_t cached_bytes{0u};
        std::size_t cache_budget_bytes{0u};
    };

    struct WorldDescriptorIndexActor final
    {
        lux::authoring::WorldActorId actor;
        uuids::uuid descriptor_page{};
        std::string display_name;
        std::string actor_class;
        lux::authoring::PartitionSpaceId space;
        WorldActorSourcePosition position;
        std::array<float, 3u> bounds_half_extent{};
        std::vector<lux::authoring::DataLayerId> data_layers;

        friend bool operator==(
            const WorldDescriptorIndexActor&,
            const WorldDescriptorIndexActor&) = default;
    };

    /// Deletable project cache. It contains no component payload and is never
    /// authoritative: every entry can be rebuilt from LXWA + LXAI.
    class LUX_ENGINE_AUTHORING_WORLD_PUBLIC WorldDescriptorIndex final
    {
    public:
        WorldDescriptorIndex();
        ~WorldDescriptorIndex();
        WorldDescriptorIndex(const WorldDescriptorIndex&) noexcept;
        WorldDescriptorIndex& operator=(
            const WorldDescriptorIndex&) noexcept;
        WorldDescriptorIndex(WorldDescriptorIndex&&) noexcept;
        WorldDescriptorIndex& operator=(WorldDescriptorIndex&&) noexcept;

        [[nodiscard]] static lux::cxx::expected<
            WorldDescriptorIndex,
            std::string>
        rebuild(
            const std::filesystem::path& world_file,
            const WorldSourceDocument& source,
            const std::filesystem::path& cache_file);

        /// Builds without IO when the caller already owns every LXAI page
        /// (new World / complete import transaction).
        [[nodiscard]] static lux::cxx::expected<
            WorldDescriptorIndex,
            std::string>
        fromPages(
            const WorldSourceDocument& source,
            std::span<const WorldDescriptorPageDocument> pages,
            const std::filesystem::path& cache_file);

        [[nodiscard]] static lux::cxx::expected<
            WorldDescriptorIndex,
            std::string>
        load(
            const std::filesystem::path& cache_file,
            const WorldSourceDocument& source);

        /// Replaces only changed pages and retains entries for untouched
        /// pages. Only affected immutable Page/Actor hash shards are rewritten;
        /// the LXDI root is committed last.
        [[nodiscard]] lux::cxx::expected<void, std::string> updatePages(
            const WorldSourceDocument& source,
            std::span<const WorldDescriptorPageDocument> changed_pages);

        [[nodiscard]] std::optional<WorldDescriptorIndexActor> find(
            lux::authoring::WorldActorId actor) const;

        [[nodiscard]] const WorldDescriptorPageReference* page(
            uuids::uuid page_id) const noexcept;

        [[nodiscard]] const WorldDescriptorPageReference* page(
            lux::authoring::PartitionSpaceId space,
            const lux::authoring::WorldMacroCoord& macro) const noexcept;

        /// Page-local lookup used by viewport streaming. Its cost is O(Actors
        /// in the requested Macro Page), never O(total World Actors).
        [[nodiscard]] std::vector<lux::authoring::WorldActorId>
            actorsInPage(
            uuids::uuid page_id) const;

        [[nodiscard]] std::vector<WorldDescriptorIndexActor> search(
            std::string_view text,
            std::size_t offset,
            std::size_t maximum) const;

        [[nodiscard]] std::size_t actorCount() const noexcept;

        [[nodiscard]] std::size_t pageCount() const noexcept;

        [[nodiscard]] lux::authoring::WorldId world() const noexcept;

        [[nodiscard]] lux::cxx::algorithm::Sha256Digest sourceDigest() const noexcept;

        /// Lightweight diagnostics; does not load an immutable object.
        [[nodiscard]] WorldDescriptorIndexStats stats() const noexcept;

    private:
        struct Data;
        friend struct WorldDescriptorIndexAccess;
        std::shared_ptr<Data> data_;
    };

    [[nodiscard]] LUX_ENGINE_AUTHORING_WORLD_PUBLIC std::filesystem::path
    worldDescriptorIndexCachePath(
        const std::filesystem::path& cache_root,
        lux::authoring::WorldId world);
} // namespace lux::authoring
