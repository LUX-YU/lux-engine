#pragma once
/**
 * @file FeatureTypeRegistry.hpp
 * @brief Renderer-owned registry of feature TYPES (factories + descriptors).
 *
 * Sunk down from the comm-layer server Impl so that dependency resolution
 * (auto-installing a feature's required dependencies) can map a stable
 * FeatureTypeId → its factory → create it, in the same layer that owns the scenes.
 *
 * NOT to be confused with comm/client/FeatureCatalog.hpp — that is a CLIENT-side
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

#include <lux/engine/function/render/client/protocol/FeatureFactory.hpp> // FeatureFactory / GenericOkReply
#include <lux/engine/function/render/client/core/FeatureTypeId.hpp>      // FeatureTypeId
#include <lux/engine/function/render/client/core/Errors.hpp>             // Expected / renderFailure
#include <lux/cxx/container/SparseSet.hpp>                               // OffsetAutoSparseSet

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view> // findByName 的重名比较
#include <vector>

namespace lux::render
{
    /// One registered feature type: how to create it (factory + descriptor) and the
    /// dynamic op TypeIds the comm layer bound for it (populated by the comm handler).
    struct FeatureTypeRecord
    {
        FeatureFactory factory{};
        std::uint32_t op_count{0};
        TypeId ops[16]{};
        /// How many register calls share this record. The idempotent AlreadyRegistered
        /// path hands a second registrant the FIRST registration's type_id + ops — i.e.
        /// shared ownership — so unregistration must be counted: one sharer's unregister
        /// must not destroy the type id and dispatcher slots the others still use.
        std::uint32_t registrations{1};
        /// One entry per registration, including an empty entry for built-ins.
        /// Keeping the lease beside the function table guarantees that a dynamic
        /// module cannot disappear while create/unregister trampolines remain
        /// callable on the render thread.
        std::vector<std::shared_ptr<const void>> registration_leases{};
        /// Live instances across every RenderScene owned by this Renderer.
        std::uint32_t active_instances{0};
    };

    /// Outcome SCOPE of a successful add(): distinguishes a fresh insert from an
    /// idempotent re-registration of the SAME factory, so a caller can tell "newly
    /// created" from "already known" without treating a legitimate re-register (e.g. a
    /// second scene wanting a type the first already registered) as a failure. The
    /// numeric values ride the wire FeatureTypeRegisteredReply::status field.
    enum class EFeatureTypeRegisterStatus : std::uint32_t
    {
        Registered = 0,        ///< newly inserted; type_id is fresh, op handlers were just bound
        AlreadyRegistered = 1, ///< same factory already present; type_id is the EXISTING id (idempotent)
    };

    /// Successful registration result: a valid type_id (>=1) plus which scope it was.
    /// Failures are carried out-of-band as the Expected's error (see add()).
    struct FeatureTypeAddResult
    {
        std::uint32_t type_id{0};
        EFeatureTypeRegisterStatus status{EFeatureTypeRegisterStatus::Registered};
    };

    class FeatureTypeRegistry
    {
    public:
        // Offset 1 so id 0 stays reserved as "none" (matches the prior comm registry).
        using Storage = lux::cxx::OffsetAutoSparseSet<std::uint32_t, FeatureTypeRecord, 1>;

        static constexpr std::uint32_t kMaxOps = 16; // FeatureTypeRecord::ops[] capacity

        /// Register a feature type (boundary-validated), returning a TYPED outcome:
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
                return renderFailure<err::feature::FactoryHasNoCreateFn>();
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
                        return renderFailure<err::feature::TypeIdCollision>(record.factory.descriptor.type);
                    // Shared ownership is COUNTED: release() destroys the record only when
                    // the last registrant lets go (see FeatureTypeRecord::registrations).
                    const std::uint32_t id = findIdByStableType(record.factory.descriptor.type);
                    auto& shared = types_.at(id);
                    ++shared.registrations;
                    if (record.registration_leases.empty())
                        shared.registration_leases.emplace_back();
                    else
                        shared.registration_leases.push_back(std::move(record.registration_leases.front()));
                    return FeatureTypeAddResult{id, EFeatureTypeRegisterStatus::AlreadyRegistered};
                }
            }

            if (record.registration_leases.empty())
                record.registration_leases.emplace_back();

            // 到这里说明是一次**新类型**的插入。在放它进来之前再拦一道:名字不能与
            // 已注册的**另一个类型**重合。
            //
            // 上面那道 TypeIdCollision 守的是 descriptor.type,而真正被当**类型判据**
            // 用过的是 name —— DeferredLightingFeature 曾按 `f->name() == "ShadowMap"`
            // 认兄弟特性然后无检查 static_cast(已改成按 type id 认人),而 comm 的
            // FeatureCatalog 至今按 name 存 op-id:撞名 = 拿到别人的 op-id。
            // `FeatureFactory.name` 按它自己的文档就是**插件自选的字符串**,所以唯一
            // 能拦住重名的地方就是这里。
            //
            // 只对有稳定 id 的类型判:descriptor-LESS 的工厂本来就走"每次插一条新记录"
            // 的历史路径(见上面的头注释),对它们判重名会把那条既有行为变成失败。
            if (record.factory.descriptor.valid() && record.factory.name != nullptr)
            {
                if (const FeatureTypeRecord* clash = findByName(record.factory.name))
                    return renderFailure<err::feature::FeatureNameCollision>(
                        encodeFeatureType(clash->factory.descriptor.type)
                    );
            }

            const std::uint32_t id = types_.insert(std::move(record));
            return FeatureTypeAddResult{id, EFeatureTypeRegisterStatus::Registered};
        }

        [[nodiscard]] bool contains(std::uint32_t id) const noexcept
        {
            return types_.contains(id);
        }
        [[nodiscard]] FeatureTypeRecord& at(std::uint32_t id)
        {
            return types_.at(id);
        }
        [[nodiscard]] const FeatureTypeRecord& at(std::uint32_t id) const
        {
            return types_.at(id);
        }
        void erase(std::uint32_t id)
        {
            types_.erase(id);
        }

        /// Counted unregistration. A final release returns the removed record so the
        /// comm layer can unbind its opcodes while the record's module lease is still
        /// alive. Any live instance rejects the release without consuming a
        /// registration reference.
        [[nodiscard]] Expected<std::optional<FeatureTypeRecord>> release(std::uint32_t id)
        {
            if (!types_.contains(id))
                return renderFailure<err::feature::TypeNotRegistered>(id);
            FeatureTypeRecord& rec = types_.at(id);
            if (rec.active_instances != 0u)
                return renderFailure<err::feature::FeatureTypeInUse>(id, rec.active_instances);
            if (rec.registrations > 1)
            {
                --rec.registrations;
                rec.registration_leases.pop_back();
                return std::optional<FeatureTypeRecord>{};
            }
            FeatureTypeRecord removed = std::move(rec);
            types_.erase(id);
            return std::optional<FeatureTypeRecord>{std::move(removed)};
        }

        void noteInstanceAdded(FeatureTypeId type) noexcept
        {
            const auto id = findIdByStableType(type);
            if (id != 0u)
                ++types_.at(id).active_instances;
        }

        void noteInstanceRemoved(FeatureTypeId type) noexcept
        {
            const auto id = findIdByStableType(type);
            if (id == 0u)
                return;
            auto& count = types_.at(id).active_instances;
            if (count != 0u)
                --count;
        }

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

        /// 按 `FeatureFactory.name` 找已注册的类型。给 add() 的重名拦截用 ——
        /// 名字被当类型判据用过,所以它必须是唯一的(见 add() 里的说明)。
        [[nodiscard]] const FeatureTypeRecord* findByName(const char* name) const noexcept
        {
            if (name == nullptr || *name == '\0')
                return nullptr;
            const std::string_view want{name};
            for (const auto& rec : types_.values())
                if (rec.factory.name != nullptr && want == rec.factory.name)
                    return &rec;
            return nullptr;
        }

        /// The dynamic id of the registered type whose descriptor has this stable type id,
        /// or 0 if none. Backs the idempotent add() path (returns the EXISTING id on a
        /// re-registration of the same feature). `keys()` and `values()` are parallel dense
        /// arrays (keys[i] ↔ values[i]). Mirrors findByStableType but yields the id.
        [[nodiscard]] std::uint32_t findIdByStableType(FeatureTypeId type) const noexcept
        {
            if (type == kInvalidFeatureTypeId)
                return 0;
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
