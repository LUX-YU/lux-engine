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

#include <lux/engine/render/comm/RenderProtocol.hpp>   // FeatureFactory, TypeId
#include <lux/engine/render/core/FeatureTypeId.hpp>     // FeatureTypeId
#include <lux/cxx/container/SparseSet.hpp>              // OffsetAutoSparseSet

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

    class FeatureTypeRegistry
    {
    public:
        // Offset 1 so id 0 stays reserved as "none" (matches the prior comm registry).
        using Storage = lux::cxx::OffsetAutoSparseSet<std::uint32_t, FeatureTypeRecord, 1>;

        static constexpr std::uint32_t kMaxOps = 16;  // FeatureTypeRecord::ops[] capacity

        /// Store a fully-built record (factory + ops already bound by the caller).
        /// Boundary-validated (五-2): REJECTS a record with no create_fn (addFeature
        /// would crash) or a duplicate stable type id (two types sharing one stable id
        /// make findByStableType / dependency resolution ambiguous) — returns 0 = "none";
        /// and CLAMPS op_count to the ops[] capacity so a misbehaving register_ops_fn
        /// can't drive an out-of-bounds copy downstream.
        /// @return the dynamic feature_type_id, or 0 if rejected.
        [[nodiscard]] std::uint32_t add(FeatureTypeRecord record)
        {
            if (record.factory.create_fn == nullptr)
                return 0;
            if (record.op_count > kMaxOps)
                record.op_count = kMaxOps;
            if (record.factory.descriptor.valid() &&
                findByStableType(record.factory.descriptor.type) != nullptr)
                return 0;
            return types_.insert(std::move(record));
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

    private:
        Storage types_;
    };

} // namespace lux::render
