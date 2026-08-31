#pragma once

#include <lux/engine/simulation/pixel/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace lux::simulation
{
    using PixelMaterialId = std::uint16_t;
    inline constexpr PixelMaterialId kEmptyPixelMaterial = 0U;

    enum class EPixelMaterialPhase : std::uint8_t
    {
        EMPTY = 0,
        SOLID,
        POWDER,
        LIQUID,
    };

    struct PixelMaterialDefinition final
    {
        EPixelMaterialPhase phase{EPixelMaterialPhase::EMPTY};
        std::uint8_t density{};
        std::uint32_t rgba8{};
    };

    struct PixelFieldConfiguration final
    {
        std::uint32_t width{};
        std::uint32_t height{};
    };

    enum class EPixelFieldError : std::uint8_t
    {
        INVALID_CONFIGURATION,
        OUT_OF_BOUNDS,
        INVALID_MATERIAL,
        MATERIAL_CAPACITY_EXCEEDED,
        ALLOCATION_FAILURE,
    };

    /// One finite, sparse, field-local cellular simulation.
    ///
    /// Large cell storage is owned by this runtime rather than by ECS
    /// components. Missing resident chunks are simulation boundaries; step()
    /// never allocates chunks. World placement, persistence, Presentation and
    /// streaming intentionally live outside this type.
    class LUX_ENGINE_SIMULATION_PIXEL_PUBLIC PixelFieldRuntime final
    {
    public:
        [[nodiscard]] static lux::cxx::expected<
            std::unique_ptr<PixelFieldRuntime>,
            EPixelFieldError
        > create(PixelFieldConfiguration configuration) noexcept;

        ~PixelFieldRuntime() noexcept;
        PixelFieldRuntime(const PixelFieldRuntime&) = delete;
        PixelFieldRuntime& operator=(const PixelFieldRuntime&) = delete;
        PixelFieldRuntime(PixelFieldRuntime&&) noexcept;
        PixelFieldRuntime& operator=(PixelFieldRuntime&&) noexcept;

        [[nodiscard]] PixelFieldConfiguration configuration() const noexcept;

        [[nodiscard]] lux::cxx::expected<PixelMaterialId, EPixelFieldError>
        addMaterial(PixelMaterialDefinition definition) noexcept;

        [[nodiscard]] const PixelMaterialDefinition& material(
            PixelMaterialId id
        ) const noexcept;

        /// Explicit mutation is the only operation that can create a resident
        /// chunk. This keeps the logical field sparse and guarantees that a
        /// steady-state step does not grow storage.
        [[nodiscard]] lux::cxx::expected<void, EPixelFieldError> setCell(
            std::int64_t x,
            std::int64_t y,
            PixelMaterialId material
        ) noexcept;

        [[nodiscard]] lux::cxx::expected<PixelMaterialId, EPixelFieldError>
        cell(std::int64_t x, std::int64_t y) const noexcept;

        /// Advances one deterministic logical tick. The initial implementation
        /// is deliberately serial; parallel execution must later prove the
        /// same state hash before it can replace this reference path.
        void step() noexcept;

        [[nodiscard]] std::uint64_t determinismHash() const noexcept;
        [[nodiscard]] std::size_t residentChunkCount() const noexcept;
        [[nodiscard]] std::size_t activeChunkCount() const noexcept;
        [[nodiscard]] std::uint64_t cellsScannedLastStep() const noexcept;
        [[nodiscard]] std::uint64_t movedCellsLastStep() const noexcept;

    private:
        explicit PixelFieldRuntime(PixelFieldConfiguration configuration);

        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::simulation
