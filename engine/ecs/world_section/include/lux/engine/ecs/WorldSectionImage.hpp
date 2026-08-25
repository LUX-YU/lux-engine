#pragma once

#include <lux/engine/ecs/WorldSectionId.hpp>
#include <lux/engine/ecs/WorldSectionTypes.hpp>
#include <lux/engine/ecs/world_section/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace lux::ecs
{
    class LUX_ENGINE_ECS_WORLD_SECTION_PUBLIC WorldSectionColumnView final
    {
      public:
        [[nodiscard]] std::uint64_t schemaHash() const noexcept;
        [[nodiscard]] std::string_view schemaName() const noexcept;
        [[nodiscard]] std::uint32_t schemaVersion() const noexcept;
        [[nodiscard]] EWorldSectionValueEncoding valueEncoding() const noexcept;
        [[nodiscard]] EWorldSectionOrdinalEncoding ordinalEncoding() const noexcept;
        [[nodiscard]] std::uint32_t rowCount() const noexcept;
        [[nodiscard]] std::uint32_t fixedStride() const noexcept;
        [[nodiscard]] std::span<const std::byte> ordinalBytes() const noexcept;
        [[nodiscard]] std::span<const std::byte> offsetBytes() const noexcept;
        [[nodiscard]] std::span<const std::byte> payload() const noexcept;

      private:
        friend class WorldSectionImage;

        std::uint64_t schema_hash_{};
        std::string_view schema_name_;
        std::uint32_t schema_version_{};
        EWorldSectionValueEncoding value_encoding_{};
        EWorldSectionOrdinalEncoding ordinal_encoding_{};
        std::uint32_t row_count_{};
        std::uint32_t fixed_stride_{};
        std::span<const std::byte> ordinal_bytes_;
        std::span<const std::byte> offset_bytes_;
        std::span<const std::byte> payload_;
    };

    class LUX_ENGINE_ECS_WORLD_SECTION_PUBLIC WorldSectionImage final
    {
      public:
        WorldSectionImage(WorldSectionImage&& other) noexcept;
        WorldSectionImage& operator=(WorldSectionImage&& other) noexcept;
        ~WorldSectionImage();

        WorldSectionImage(const WorldSectionImage&) = delete;
        WorldSectionImage& operator=(const WorldSectionImage&) = delete;

        [[nodiscard]] static lux::cxx::expected<
            WorldSectionImage,
            WorldSectionFailure>
        open(
            std::vector<std::byte> bytes,
            WorldSectionLimits limits = {}
        ) noexcept;

        [[nodiscard]] const WorldSectionId& id() const noexcept;
        [[nodiscard]] std::uint32_t entityCount() const noexcept;
        [[nodiscard]] std::span<const WorldSectionColumnView> columns() const noexcept;
        [[nodiscard]] std::span<const std::byte> bytes() const noexcept;

      private:
        WorldSectionImage() = default;

        WorldSectionId id_;
        std::uint32_t entity_count_{};
        std::vector<std::byte> bytes_;
        std::vector<WorldSectionColumnView> columns_;
    };
} // namespace lux::ecs
