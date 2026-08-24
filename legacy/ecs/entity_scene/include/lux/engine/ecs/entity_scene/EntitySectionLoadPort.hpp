#pragma once
/**
 * @file EntitySectionLoadPort.hpp
 * @brief Typed loading port consumed by the EntitySection ECS System.
 */

#include <lux/engine/core/async/OperationPort.hpp>
#include <lux/engine/ecs/entity_scene/EntityBatchDecoder.hpp>
#include <lux/engine/ecs/scene_format/SceneSectionManifest.hpp>
#include <lux/engine/resource/asset/storage/AssetVfs.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace lux::ecs::entity_scene
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
        lux::ecs::scene_format::SectionRecord record;
        std::uint64_t request_generation{0u};
    };

    namespace detail
    {
        using SupportsGeneratedSectionFn = bool (*)(
            const void*,
            const lux::ecs::scene_format::SectionGeneratorId&) noexcept;

        struct EntitySectionLoadState final
        {
            std::atomic<bool> closing{false};
            std::shared_ptr<const void> generator_provider;
            SupportsGeneratedSectionFn supports_generated{nullptr};
        };
    }

    /// Narrow capability value. It carries one typed submit endpoint and a
    /// read-only source-support predicate; it owns no scheduler or queue.
    class EntitySectionLoadPort final
    {
    public:
        EntitySectionLoadPort() noexcept = default;

        EntitySectionLoadPort(
            std::weak_ptr<detail::EntitySectionLoadState> state,
            lux::async::OperationPort<LoadEntitySection> operation) noexcept
            : state_(std::move(state)), operation_(std::move(operation))
        {}

        [[nodiscard]] LoadEntitySection loadOperation(
            std::shared_ptr<const lux::asset::AssetVfs> vfs,
            lux::ecs::scene_format::SectionRecord record,
            std::uint64_t request_generation) const noexcept
        {
            return {
                std::move(vfs),
                std::move(record),
                request_generation};
        }

        [[nodiscard]] const lux::async::OperationPort<LoadEntitySection>&
        operation() const noexcept
        {
            return operation_;
        }

        [[nodiscard]] explicit operator bool() const noexcept
        {
            const auto state = state_.lock();
            return state &&
                !state->closing.load(std::memory_order_acquire) &&
                static_cast<bool>(operation_);
        }

        [[nodiscard]] bool supports(
            const lux::ecs::scene_format::SectionRecord& record) const
            noexcept
        {
            const auto state = state_.lock();
            if (!state || state->closing.load(std::memory_order_acquire) ||
                !operation_)
            {
                return false;
            }
            if (std::holds_alternative<
                    lux::ecs::scene_format::StoredSectionSource>(
                    record.source))
            {
                return true;
            }
            const auto& generated = std::get<
                lux::ecs::scene_format::GeneratedSectionSource>(
                record.source);
            return state->generator_provider && state->supports_generated &&
                state->supports_generated(
                    state->generator_provider.get(),
                    generated.generator);
        }

    private:
        std::weak_ptr<detail::EntitySectionLoadState> state_;
        lux::async::OperationPort<LoadEntitySection> operation_;
    };
} // namespace lux::ecs::entity_scene
