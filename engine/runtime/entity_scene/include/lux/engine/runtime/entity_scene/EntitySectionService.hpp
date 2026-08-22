#pragma once
/**
 * @file EntitySectionService.hpp
 * @brief Bounded, typed LXES loading operation for EntitySection owners.
 */

#include <lux/engine/resource/asset/storage/AssetVfs.hpp>
#include <lux/engine/scene/SceneDescription.hpp>
#include <lux/engine/runtime/entity_scene/EntityBatchDecoder.hpp>
#include <lux/engine/runtime/entity_scene/EntitySectionGeneratorCatalog.hpp>
#include <lux/engine/runtime/entity_scene/visibility.h>
#include <lux/engine/runtime/execution/AsyncRuntimeBuilder.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace lux::runtime::entity_scene
{
    inline constexpr std::size_t kEntitySectionLoadQueueCapacity = 16u;
    inline constexpr std::size_t kEntitySectionLoadByteBudget =
        256u * 1024u * 1024u;
    inline constexpr std::size_t kEntitySectionLoadDrainBatch = 4u;

    enum class EEntitySectionLoadError : std::uint8_t
    {
        INVALID_REQUEST,
        SERVICE_CLOSED,
        SECTION_NOT_FOUND,
        IO_FAILED,
        GENERATOR_NOT_FOUND,
        GENERATION_FAILED,
        ENCODE_FAILED,
        ENCODED_SIZE_MISMATCH,
        DECOMPRESSION_FAILED,
        DECODED_SIZE_MISMATCH,
        DIGEST_MISMATCH,
        DECODE_FAILED,
        RECORD_MISMATCH
    };

    struct EntitySectionLoadResult final
    {
        std::uint64_t request_generation{0u};
        DecodedEntityBatch decoded;

        EntitySectionLoadResult(
            std::uint64_t generation,
            DecodedEntityBatch value) noexcept
            : request_generation(generation), decoded(std::move(value))
        {}

        EntitySectionLoadResult(EntitySectionLoadResult&&) noexcept = default;
        EntitySectionLoadResult& operator=(
            EntitySectionLoadResult&&) noexcept = default;
        EntitySectionLoadResult(const EntitySectionLoadResult&) = delete;
        EntitySectionLoadResult& operator=(
            const EntitySectionLoadResult&) = delete;
    };

    struct LoadEntitySection final
    {
        using Value = EntitySectionLoadResult;
        using Error = EEntitySectionLoadError;

        std::shared_ptr<const lux::asset::AssetVfs> vfs;
        lux::scene::SectionRecord record;
        std::uint64_t request_generation{0u};
    };

    namespace detail
    {
        struct EntitySectionOperationControl final
        {
            std::atomic<bool> closing{false};
            std::shared_ptr<const EntitySectionGeneratorCatalog> generators;
        };
    }

    class LUX_ENGINE_RUNTIME_ENTITY_SCENE_PUBLIC EntitySectionLoadClient final
    {
    public:
        EntitySectionLoadClient() noexcept = default;

        [[nodiscard]] LoadEntitySection loadOperation(
            std::shared_ptr<const lux::asset::AssetVfs> vfs,
            lux::scene::SectionRecord record,
            std::uint64_t request_generation) const noexcept;

        [[nodiscard]] const lux::async::OperationPort<
            LoadEntitySection>& operation() const noexcept
        {
            return operation_;
        }

        [[nodiscard]] explicit operator bool() const noexcept;

        /// Side-effect-free source admission used before a scene acquires any
        /// startup ticket. Stored sources are universally supported; a
        /// generated source requires its full canonical ID in the frozen
        /// catalog.
        [[nodiscard]] bool supports(
            const lux::scene::SectionRecord& record) const
            noexcept;

    private:
        friend class EntitySectionService;

        EntitySectionLoadClient(
            std::weak_ptr<detail::EntitySectionOperationControl> control,
            lux::async::OperationPort<LoadEntitySection> operation)
            noexcept;

        std::weak_ptr<detail::EntitySectionOperationControl> control_;
        lux::async::OperationPort<LoadEntitySection> operation_;
    };

    /// Process-level operation registration. Scene residency and ECS
    /// publication are intentionally owned by EntitySectionLoaderSystem.
    class LUX_ENGINE_RUNTIME_ENTITY_SCENE_PUBLIC EntitySectionService final
    {
    public:
        [[nodiscard]] static lux::cxx::expected<
            EntitySectionService,
            lux::exec::AsyncAssemblyFailure>
        addTo(
            lux::exec::AsyncRuntimeBuilder& builder,
            std::shared_ptr<const EntitySectionGeneratorCatalog> generators =
                {});

        EntitySectionService(const EntitySectionService&) = delete;
        EntitySectionService& operator=(const EntitySectionService&) = delete;
        EntitySectionService(EntitySectionService&& other) noexcept;
        EntitySectionService& operator=(EntitySectionService&& other) noexcept;
        ~EntitySectionService();

        [[nodiscard]] EntitySectionLoadClient loadClient() const noexcept;
        void close() noexcept;

    private:
        EntitySectionService(
            std::shared_ptr<detail::EntitySectionOperationControl> control,
            lux::async::OperationPort<LoadEntitySection> operation)
            noexcept;

        std::shared_ptr<detail::EntitySectionOperationControl> control_;
        lux::async::OperationPort<LoadEntitySection> operation_;
        bool closed_{false};
    };
}
