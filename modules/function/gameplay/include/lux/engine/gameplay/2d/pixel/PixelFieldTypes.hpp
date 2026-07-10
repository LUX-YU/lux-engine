#pragma once
// ============================================================================
//  PixelFieldTypes.hpp — the pixel-field domain's comm-free vocabulary
//  (lux::gameplay::d2, F2-00/F2-02/F2-03/F2-06).
//
//  Everything here is a POD or a pure function: the generational field handle,
//  the world↔cell coordinate contract, the material schema, and the
//  query/command/event boundary types. The mutable state lives in
//  PixelFieldRuntime (NEVER in ECS components — cell counts must not touch
//  entity counts, F2-00).
//
//  COORDINATE CONTRACT (F2-02, frozen):
//   - a field is an axis-aligned cell grid; cell (0,0) is the BOTTOM-LEFT cell,
//     +x right, +y UP (gravity pulls toward row y-1);
//   - MVP world mapping is TRANSLATION-ONLY (rotation = 0, scale = 1): the
//     owning entity's WorldTransform2D translation is the world position of the
//     field's min corner; `cell_size` is the single grid scale;
//   - worldToCell uses mathematical FLOOR (negative coordinates round toward
//     -infinity — floorDiv below is the shared helper, also for chunk math).
// ============================================================================

#include <lux/cxx/container/SlotMap.hpp>   // lux::cxx::SlotKey (generational handle)
#include <Eigen/Core>

#include <cmath>
#include <cstdint>
#include <vector>

namespace lux::gameplay::d2
{
    // ── F2-00: handle ───────────────────────────────────────────────────────
    struct PixelFieldTag{};
    /// Generational handle into PixelFieldRuntime's slot map. A stale handle
    /// (generation mismatch) is rejected by every runtime API.
    using PixelFieldHandle = lux::cxx::SlotKey<PixelFieldTag>;

    // ── F2-02: coordinates ──────────────────────────────────────────────────

    /// Mathematical floor division/modulo (negative-correct; C++ '/' truncates).
    [[nodiscard]] constexpr std::int32_t floorDiv(std::int32_t a, std::int32_t b) noexcept
    {
        const std::int32_t q = a / b;
        return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
    }
    [[nodiscard]] constexpr std::int32_t floorMod(std::int32_t a, std::int32_t b) noexcept
    {
        return a - floorDiv(a, b) * b;
    }

    /// The world placement of one field: min-corner world position + cell size.
    /// (Derived per frame from the owning entity's WorldTransform2D translation;
    /// rotation/scale are FORBIDDEN in the MVP contract.)
    struct PixelFieldFrame
    {
        Eigen::Vector2f origin{0.f, 0.f};   ///< world position of cell (0,0)'s min corner
        float           cell_size{0.1f};    ///< world units per cell (> 0)
    };

    [[nodiscard]] inline Eigen::Vector2i worldToCell(const PixelFieldFrame& f,
                                                     const Eigen::Vector2f& world) noexcept
    {
        return {static_cast<std::int32_t>(std::floor((world.x() - f.origin.x()) / f.cell_size)),
                static_cast<std::int32_t>(std::floor((world.y() - f.origin.y()) / f.cell_size))};
    }
    [[nodiscard]] inline Eigen::Vector2f cellToWorldMin(const PixelFieldFrame& f,
                                                        const Eigen::Vector2i& cell) noexcept
    {
        return f.origin + Eigen::Vector2f(cell.x(), cell.y()) * f.cell_size;
    }
    [[nodiscard]] inline Eigen::Vector2f cellToWorldCenter(const PixelFieldFrame& f,
                                                           const Eigen::Vector2i& cell) noexcept
    {
        return cellToWorldMin(f, cell) + Eigen::Vector2f(0.5f, 0.5f) * f.cell_size;
    }

    // ── F2-03: material schema ──────────────────────────────────────────────

    using MaterialId = std::uint16_t;
    inline constexpr MaterialId kEmptyMaterial = 0;

    /// The simulation-facing phase. Static params (density, colour) live in the
    /// material TABLE only — never per cell.
    enum class EMaterialPhase : std::uint8_t
    {
        Empty = 0,   ///< nothing; the universal move target
        Solid,       ///< static (stone, wood)
        Powder,      ///< falls + piles (sand); displaces lighter liquids
        Liquid,      ///< falls + spreads laterally (water)
    };

    struct MaterialDef
    {
        EMaterialPhase phase{EMaterialPhase::Empty};
        std::uint8_t   density{0};            ///< arbitration: heavier displaces lighter liquid
        std::uint32_t  color{0x00000000u};    ///< premultiplied RGBA8 palette entry (F2-09)
    };

    /// Static material table. Id 0 is ALWAYS the empty material. Define the set
    /// before creating fields; the table is append-only (ids are frozen into
    /// cells and into the GPU palette).
    class PixelMaterialRegistry
    {
    public:
        PixelMaterialRegistry() { defs_.push_back(MaterialDef{}); }   // id 0 = empty
        MaterialId add(const MaterialDef& def)
        {
            defs_.push_back(def);
            return static_cast<MaterialId>(defs_.size() - 1);
        }
        [[nodiscard]] const MaterialDef& at(MaterialId id) const noexcept
        {
            return id < defs_.size() ? defs_[id] : defs_[0];
        }
        [[nodiscard]] std::uint32_t count() const noexcept
        {
            return static_cast<std::uint32_t>(defs_.size());
        }

    private:
        std::vector<MaterialDef> defs_;
    };

    /// Optional per-cell channels (F2-03). The BASE plane (MaterialId) always
    /// exists; a channel plane is allocated ONLY when its bit is set in
    /// PixelFieldDesc::channels_mask — a disabled channel costs nothing.
    /// MVP: reserved (no rule consumes them yet); the enum + allocation gate is
    /// the frozen seam.
    enum class ECellChannel : std::uint8_t
    {
        Temperature = 0,
        Lifetime    = 1,
        Count
    };
    [[nodiscard]] constexpr std::uint32_t channelBit(ECellChannel c) noexcept
    {
        return 1u << static_cast<std::uint32_t>(c);
    }

    /// Creation descriptor. Single-screen dense grid (chunking is C2-*).
    struct PixelFieldDesc
    {
        std::uint32_t cells_w{0};
        std::uint32_t cells_h{0};
        std::uint32_t channels_mask{0};   ///< ECellChannel bits; 0 = base plane only
    };

    // ── F2-06: query / command / event boundary ─────────────────────────────

    struct PixelRaycastHit
    {
        bool            hit{false};
        Eigen::Vector2i cell{0, 0};
        MaterialId      material{kEmptyMaterial};
    };

    /// Deferred field mutation. ALL writes go through commands (the public API
    /// exposes no mutable cell access), applied FIFO at the fixed-step
    /// ApplyFieldCommands phase — deterministic order, never mid-scan.
    struct PixelFieldCommand
    {
        enum class EKind : std::uint8_t
        {
            StampRect = 0,   ///< set every cell in [min, min+size) to `material`
                             ///< (erase = stamp kEmptyMaterial)
        };
        EKind            kind{EKind::StampRect};
        PixelFieldHandle field{};
        Eigen::Vector2i  min{0, 0};
        Eigen::Vector2i  size{0, 0};
        MaterialId       material{kEmptyMaterial};
    };

    /// C2-03: one hit of a world-space field query — the handle plus the frame
    /// SNAPSHOT the runtime cached at the last fixed step's owner maintenance.
    /// The frame lags live ECS writes by at most one substep (the cache is a
    /// routing convenience, never a second truth source — the authoritative
    /// placement stays the owning entity's WorldTransform2D). Fields are not
    /// expected to move fast; a consumer needing exact placement reads the
    /// component. MVP forbids overlapping fields (R-08) — overlaps are
    /// detected and warned, the query still returns every intersecting field
    /// in priority order.
    struct PixelFieldQueryEntry
    {
        PixelFieldHandle handle{};
        PixelFieldFrame  frame{};
        float            priority{0.f};
    };

    /// Facts the simulation reports outward (drained by PublishEvents / callers).
    struct PixelFieldEvent
    {
        enum class EKind : std::uint8_t
        {
            CommandsApplied = 0,   ///< a command batch landed; cells_changed cells differ
            FieldDestroyed,        ///< the field died (render caches must evict)
        };
        EKind            kind{EKind::CommandsApplied};
        PixelFieldHandle field{};
        std::uint32_t    cells_changed{0};
    };

} // namespace lux::gameplay::d2
