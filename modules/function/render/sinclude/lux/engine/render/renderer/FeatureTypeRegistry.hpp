#pragma once
/**
 * @file FeatureTypeRegistry.hpp
 * @brief Renderer-owned registry of feature TYPES (factories + descriptors).
 *
 * 阶段 3: sunk down from the comm-layer server Impl so that dependency resolution
 * (auto-installing a feature's required dependencies) can map a stable
 * FeatureTypeId → its factory → create it, in the same layer that owns the scenes.
 *
 * NOT to be confused with comm/server/FeatureRegistry.hpp — that is a CLIENT-side
 * name→{handle, ops} helper for the editor. This is the SERVER/renderer-side store
 * of "what feature types exist and how to create them".
 *
 * Split of concerns with the comm layer:
 *   - This registry OWNS the FeatureTypeRecord storage + the stable-id lookup.
 *   - The comm layer still drives op register/unregister (it holds the Dispatcher);
 *     it populates the record's ops[] before adding, and reads them back after.
 *
 * The dynamic uint32 id (insert index, ≥1) remains the comm wire `feature_type_id`;
 * `findByStableType()` is the NEW capability that dependency resolution needs.
 */

#include <lux/engine/render/comm/RenderProtocol.hpp>   // FeatureFactory, FeatureCreateFn, TypeId
#include <lux/engine/render/core/FeatureTypeId.hpp>     // FeatureTypeId
#include <lux/engine/render/core/Errors.hpp>            // Expected, ERenderError, make_error_code
#include <lux/cxx/container/SparseSet.hpp>              // OffsetAutoSparseSet

#include <cstddef>
#include <cstdint>

namespace lux::render
{
    /// One registered feature type: how to create it (factory + descriptor) and the
    /// dynamic op TypeIds the comm layer bound for it (populated by the comm handler).
    struct FeatureTypeRecord
    {
        FeatureFactory factory{};
        std::uint32_t  op_count{0};
        TypeId         ops[16]{};
    };

    /// Outcome SCOPE of a successful add(): distinguishes a fresh insert from an
    /// idempotent re-registration of the SAME factory, so a caller can tell "newly
    /// created" from "already known" without treating a legitimate re-register (e.g. a
    /// second scene wanting a type the first already registered) as a failure. The
    /// numeric values ride the wire FeatureTypeRegisteredReply::status field.
    enum class EFeatureTypeRegisterStatus : std::uint32_t
    {
        Registered        = 0,   ///< newly inserted; type_id is fresh, op handlers were just bound
        AlreadyRegistered = 1,   ///< same factory already present; type_id is the EXISTING id (idempotent)
    };

    /// Successful registration result: a valid type_id (>=1) plus which scope it was.
    /// Failures are carried out-of-band as the Expected's error (see add()).
    struct FeatureTypeAddResult
    {
        std::uint32_t              type_id{0};
        EFeatureTypeRegisterStatus status{EFeatureTypeRegisterStatus::Registered};
    };

    class FeatureTypeRegistry
    {
    public:
        // Offset 1 so id 0 stays reserved as "none" (matches the prior comm registry).
        using Storage = lux::cxx::OffsetAutoSparseSet<std::uint32_t, FeatureTypeRecord, 1>;

        static constexpr std::uint32_t kMaxOps = 16;  // FeatureTypeRecord::ops[] capacity

        /// Register a feature type (五-2 boundary-validated), returning a TYPED outcome:
        ///   - Registered        — a genuinely new type was inserted (fresh type_id).
        ///   - AlreadyRegistered — a type with the SAME stable id is already present; the
        ///                         EXISTING type_id is returned, idempotently. This is the
        ///                         normal case when a second scene re-registers a type the
        ///                         first already registered — NOT a failure. (Only descriptor'd
        ///                         factories have a stable id to dedup by.)
        /// Failures are the Expected's error:
        ///   - NullFeatureFactory   — no create_fn (addFeature would later crash).
        ///   - FeatureTypeCollision — a DIFFERENT factory (create_fn) claims an already-
        ///                            registered stable type id (findByStableType / dependency
        ///                            resolution would be ambiguous).
        /// op_count is CLAMPED to the ops[] capacity so a misbehaving register_ops_fn can't
        /// drive an out-of-bounds copy downstream. A descriptor-LESS factory has no stable id,
        /// so a re-registration still inserts a fresh record (bounded over-insert; the durable
        /// fix is to give those factories real stable descriptors).
        [[nodiscard]] Expected<FeatureTypeAddResult> add(FeatureTypeRecord record)
        {
            if (record.factory.create_fn == nullptr)
                return lux::cxx::unexpected(make_error_code(ERenderError::NullFeatureFactory));
            if (record.op_count > kMaxOps)
                record.op_count = kMaxOps;

            // Idempotent dedup is by STABLE TYPE (the descriptor's identity), which is what
            // uniquely names a feature type. create_fn is NOT a valid identity: distinct
            // feature types legitimately share one create trampoline (e.g. a generic factory
            // or a test mock), so keying on it would wrongly merge them. Descriptor-LESS
            // factories have no stable identity, so they keep the historical "insert a fresh
            // record" behavior (a known bounded over-insert — see the header note).
            if (record.factory.descriptor.valid())
            {
                if (const FeatureTypeRecord* existing = findByStableType(record.factory.descriptor.type))
                {
                    // Same stable id: the SAME feature re-registered (idempotent) — UNLESS a
                    // DIFFERENT factory is claiming it (create_fn differs), a genuine collision
                    // that would make findByStableType / dependency resolution ambiguous.
                    if (existing->factory.create_fn != record.factory.create_fn)
                        return lux::cxx::unexpected(make_error_code(ERenderError::FeatureTypeCollision));
                    return FeatureTypeAddResult{findIdByStableType(record.factory.descriptor.type),
                                                EFeatureTypeRegisterStatus::AlreadyRegistered};
                }
            }

            const std::uint32_t id = types_.insert(std::move(record));
            return FeatureTypeAddResult{id, EFeatureTypeRegisterStatus::Registered};
        }

        [[nodiscard]] bool                contains(std::uint32_t id) const noexcept { return types_.contains(id); }
        [[nodiscard]] FeatureTypeRecord&  at(std::uint32_t id)                      { return types_.at(id); }
        [[nodiscard]] const FeatureTypeRecord& at(std::uint32_t id) const           { return types_.at(id); }
        void                              erase(std::uint32_t id)                   { types_.erase(id); }

        /// Resolve a declared dependency: the registered type whose descriptor has
        /// this stable id, or nullptr if no such type is registered. The engine of
        /// dependency auto-install (slice 3c). kInvalidFeatureTypeId never matches.
        // Read-only: dependency resolution only needs the factory (create_fn /
        // descriptor). values() is a const dense view, so the result is const too.
        [[nodiscard]] const FeatureTypeRecord* findByStableType(FeatureTypeId type) const noexcept
        {
            if (type == kInvalidFeatureTypeId)
                return nullptr;
            for (const auto& rec : types_.values())
                if (rec.factory.descriptor.type == type)
                    return &rec;
            return nullptr;
        }

        /// The dynamic id of the registered type whose descriptor has this stable type id,
        /// or 0 if none. Backs the idempotent add() path (returns the EXISTING id on a
        /// re-registration of the same feature). `keys()` and `values()` are parallel dense
        /// arrays (keys[i] ↔ values[i]). Mirrors findByStableType but yields the id.
        [[nodiscard]] std::uint32_t findIdByStableType(FeatureTypeId type) const noexcept
        {
            if (type == kInvalidFeatureTypeId) return 0;
            const auto& ks = types_.keys();
            const auto& vs = types_.values();
            for (std::size_t i = 0; i < vs.size(); ++i)
                if (vs[i].factory.descriptor.type == type)
                    return ks[i];
            return 0;
        }

    private:
        Storage types_;
    };

} // namespace lux::render
