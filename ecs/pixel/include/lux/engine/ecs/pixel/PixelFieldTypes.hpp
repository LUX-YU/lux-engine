#pragma once

#include <lux/cxx/container/SlotMap.hpp>

#include <lux/engine/ecs/pixel/PixelFieldId.hpp>
#include <lux/engine/math/Position.hpp>
#include <lux/engine/math/Grid.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

namespace lux::ecs
{
    struct PixelFieldTag final {};
    using PixelFieldHandle = lux::cxx::SlotKey<PixelFieldTag>;
    using PixelChunkCoord = lux::math::GridCoord2i64;

    struct PixelChunkCoordHash final
    {
        [[nodiscard]] std::size_t operator()(
            const PixelChunkCoord& value) const noexcept
        {
            auto mix = [](std::uint64_t v) noexcept
            {
                v ^= v >> 30u;
                v *= 0xbf58476d1ce4e5b9ull;
                v ^= v >> 27u;
                v *= 0x94d049bb133111ebull;
                return v ^ (v >> 31u);
            };
            return static_cast<std::size_t>(
                mix(static_cast<std::uint64_t>(value.x)) ^
                (mix(static_cast<std::uint64_t>(value.y)) << 1u));
        }
    };

    struct PixelChunkCoordEqual final
    {
        [[nodiscard]] bool operator()(
            const PixelChunkCoord& left,
            const PixelChunkCoord& right) const noexcept
        {
            return left.x == right.x && left.y == right.y;
        }
    };

    struct PixelCellCoord final
    {
        std::int64_t x{0};
        std::int64_t y{0};

        friend bool operator==(
            const PixelCellCoord&,
            const PixelCellCoord&) = default;
    };

    struct PixelCellExtent final
    {
        std::uint32_t width{0u};
        std::uint32_t height{0u};
    };

    struct PixelChunkBounds final
    {
        PixelChunkCoord minimum{};
        PixelChunkCoord maximum{};

        [[nodiscard]] bool valid() const noexcept
        {
            return minimum.x <= maximum.x && minimum.y <= maximum.y;
        }
        [[nodiscard]] bool contains(PixelChunkCoord coordinate) const noexcept
        {
            return valid() && coordinate.x >= minimum.x &&
                coordinate.y >= minimum.y && coordinate.x <= maximum.x &&
                coordinate.y <= maximum.y;
        }

        friend bool operator==(
            const PixelChunkBounds&,
            const PixelChunkBounds&) = default;
    };

    enum class EPixelFieldExtent : std::uint8_t
    {
        BOUNDED,
        INFINITE_FIELD
    };

    struct PixelFieldFrame final
    {
        lux::math::Position2d origin{};
        float cell_size{0.1f};
    };

    /// Converts a public double position to the sparse integer cell lattice.
    /// A result outside int64 cells is rejected.
    [[nodiscard]] inline std::optional<PixelCellCoord> worldToCell(
        const PixelFieldFrame& frame,
        const lux::math::Position2d& world) noexcept
    {
        if (!(frame.cell_size > 0.0f) ||
            !lux::math::isFinite(frame.origin) ||
            !lux::math::isFinite(world))
        {
            return std::nullopt;
        }
        const auto dx = static_cast<long double>(world.x) - frame.origin.x;
        const auto dy = static_cast<long double>(world.y) - frame.origin.y;
        const auto x = std::floor(dx / frame.cell_size);
        const auto y = std::floor(dy / frame.cell_size);
        // On MSVC long double is binary64.  Compare against the exact
        // half-open int64 range instead of converting INT64_MAX, which rounds
        // to 2^63 and would admit that out-of-range endpoint.
        constexpr long double kInt64Minimum = -9223372036854775808.0L;
        constexpr long double kInt64Limit = 9223372036854775808.0L;
        if (x < kInt64Minimum || x >= kInt64Limit ||
            y < kInt64Minimum || y >= kInt64Limit)
        {
            return std::nullopt;
        }
        return PixelCellCoord{
            static_cast<std::int64_t>(x),
            static_cast<std::int64_t>(y)};
    }

    using MaterialId = std::uint16_t;
    inline constexpr MaterialId kEmptyMaterial = 0u;

    enum class EMaterialPhase : std::uint8_t
    {
        EMPTY = 0,
        SOLID,
        POWDER,
        LIQUID
    };

    struct MaterialDef final
    {
        EMaterialPhase phase{EMaterialPhase::EMPTY};
        std::uint8_t density{0u};
        std::uint32_t color{0x00000000u};
    };

    class PixelMaterialRegistry final
    {
    public:
        PixelMaterialRegistry() { definitions_.push_back({}); }

        [[nodiscard]] MaterialId add(const MaterialDef& definition)
        {
            if (definitions_.size() >=
                std::numeric_limits<MaterialId>::max())
            {
                return kEmptyMaterial;
            }
            definitions_.push_back(definition);
            return static_cast<MaterialId>(definitions_.size() - 1u);
        }

        [[nodiscard]] const MaterialDef& at(MaterialId id) const noexcept
        {
            return id < definitions_.size()
                ? definitions_[id]
                : definitions_.front();
        }

        [[nodiscard]] std::uint32_t count() const noexcept
        {
            return static_cast<std::uint32_t>(definitions_.size());
        }

    private:
        std::vector<MaterialDef> definitions_;
    };

    enum class ECellChannel : std::uint8_t
    {
        TEMPERATURE = 0,
        LIFETIME,
        COUNT
    };

    [[nodiscard]] constexpr std::uint32_t channelBit(
        ECellChannel channel) noexcept
    {
        return 1u << static_cast<std::uint32_t>(channel);
    }

    struct PixelFieldDesc final
    {
        // Optional content identity used by legacy/content leaf adapters.
        // Runtime handle identity is always the SlotMap key; this UUID does
        // not impose uniqueness and authored ECS components never expose it.
        PixelFieldId id;
        EPixelFieldExtent extent{EPixelFieldExtent::BOUNDED};
        PixelChunkBounds bounds{};
        std::uint32_t channels_mask{0u};
    };

    struct PixelFieldCommand final
    {
        enum class EKind : std::uint8_t
        {
            STAMP_RECT = 0,
            STAMP_CELLS
        };

        EKind kind{EKind::STAMP_RECT};
        PixelFieldHandle field{};
        PixelCellCoord minimum{};
        PixelCellExtent extent{};
        MaterialId material{kEmptyMaterial};
        std::shared_ptr<const std::vector<MaterialId>> cells{};
    };

    struct PixelFieldQueryEntry final
    {
        PixelFieldHandle handle{};
        PixelFieldFrame frame{};
        float priority{0.0f};
    };

    struct PixelFieldEvent final
    {
        enum class EKind : std::uint8_t
        {
            COMMANDS_APPLIED = 0,
            CHUNK_LOADED,
            CHUNK_UNLOADED,
            FIELD_DESTROYED
        };

        EKind kind{EKind::COMMANDS_APPLIED};
        PixelFieldHandle field{};
        PixelChunkCoord chunk{};
        std::uint32_t cells_changed{0u};
    };
} // namespace lux::ecs
