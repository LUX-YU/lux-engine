#pragma once

#include <lux/engine/ecs/ComponentCodec.hpp>
#include <lux/engine/ecs/persistence/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lux::ecs
{
    struct TaggedPropertyLimits final
    {
        std::uint32_t max_properties{4096};
        std::uint32_t max_property_bytes{16U * 1024U * 1024U};
    };

    class LUX_ENGINE_ECS_PERSISTENCE_PUBLIC TaggedPropertyWriter final
    {
      public:
        TaggedPropertyWriter(
            std::vector<std::byte>& destination,
            std::vector<std::string>& name_table
        ) noexcept;

        [[nodiscard]] lux::cxx::expected<void, EComponentCodecError>
        write(
            std::string_view name,
            EComponentWireType type,
            std::span<const std::byte> bytes
        ) noexcept;

        [[nodiscard]] lux::cxx::expected<void, EComponentCodecError>
        finish() noexcept;

        [[nodiscard]] std::uint32_t lastPayloadOffset() const noexcept
        {
            return last_payload_offset_;
        }

      private:
        std::vector<std::byte>* destination_{};
        std::vector<std::string>* names_{};
        std::uint32_t property_count_{};
        std::uint32_t last_payload_offset_{};
        bool finished_{};
        bool allocation_failed_{};
    };

    class LUX_ENGINE_ECS_PERSISTENCE_PUBLIC TaggedPropertyReader final
    {
      public:
        TaggedPropertyReader(
            std::span<const std::byte> bytes,
            std::span<const std::string> name_table,
            TaggedPropertyLimits limits = {}) noexcept;

        [[nodiscard]] bool next(EncodedPropertyView& property) noexcept;
        [[nodiscard]] bool valid() const noexcept;

      private:
        std::span<const std::byte> bytes_;
        std::span<const std::string> names_;
        TaggedPropertyLimits limits_;
        std::size_t offset_{};
        std::uint32_t remaining_{};
        bool valid_{true};
    };
} // namespace lux::ecs
