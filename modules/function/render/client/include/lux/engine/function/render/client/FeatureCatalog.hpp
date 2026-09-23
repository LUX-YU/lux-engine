#pragma once
// ============================================================================
//  FeatureCatalog.hpp — 进程域特性目录 + 每场景绑定表 + 子系统取用视图。
//
//  FeatureCatalog(进程域,装配后只读):name → {服务端动态 type_id, 动态
//  op-ids, param_set_op, FeatureDescriptor}。register an ORDERED set of
//  factories (built-in OR plugin), then look up each feature's dynamic op-ids
//  BY NAME (== FeatureFactory.name == RenderFeature::name())。A plugin just
//  add()s its own factory — the editor discovers + addresses it with ZERO
//  compile-time coupling。**声明序即 attach 并列决胜序**(见 resolver)。
// ============================================================================

#include <lux/engine/function/render/client/core/FeatureHandle.hpp>
#include <lux/engine/function/render/client/core/RenderFeatureRegistration.hpp> // FeatureFactory / FeatureDescriptor / TypeId

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lux::render
{
class FeatureCatalog
{
  public:
    /// Record @p factory 的注册结果:@p server_type_id 与 @p ops 是调用方
    /// 在服务端线程上 `server.addFeatureFactory(factory)` 的回执。名字为空
    /// 的工厂**拒收**(目录按名字寻址,空名条目谁也找不到 —— 例如 ImGui 的
    /// 内部工厂本就不该进目录)。
    [[nodiscard]] Expected<void> add(const RenderFeatureRegistration &registration, std::uint32_t server_type_id,
                                    std::span<const TypeId> ops)
    {
        const auto& factory = registration.factory;
        const bool invalid = factory.name == nullptr || factory.name[0] == '\0' || server_type_id == 0 ||
            ops.size() > 16 || ops.size() != factory.operation_count;
        if (invalid)
            return renderFailure<err::feature::InvalidRegistration>();
        if (find(factory.name) || (factory.descriptor.valid() && find(factory.descriptor.type)))
            return renderFailure<err::feature::TypeIdCollision>(factory.descriptor.type);
        Entry e{};
        e.registration = registration;
        e.name = factory.name;
        e.feature_type_id = server_type_id;
        e.op_count = static_cast<std::uint32_t>(ops.size());
        std::copy(ops.begin(), ops.end(), e.ops);
        e.param_set_op_index = factory.param_set_op_index;
        entries_.push_back(std::move(e));
        return {};
    }

    [[nodiscard]] Expected<void> add(const FeatureFactory &factory, std::uint32_t server_type_id,
                                    std::span<const TypeId> ops)
    {
        return add(RenderFeatureRegistration{factory, {}, false}, server_type_id, ops);
    }

    // ── Lookups by name (== RenderFeature::name()) ──

    /// The server-registered feature TYPE id (0 if absent). Type ids are
    /// process-scoped; per-scene attach (client addFeature) addresses types
    /// by this id — the returned per-scene handle lands in FeatureBindings.
    [[nodiscard]] std::uint32_t typeId(std::string_view name) const
    {
        const Entry *e = find(name);
        return e ? e->feature_type_id : 0u;
    }

    /// Typed op-ids — e.g. ops<LightOperationIds>("Light"). Returns IdsT{}
    /// (invalid) when the feature is absent, so the matching XxxProxy no-ops.
    template <class IdsT> [[nodiscard]] IdsT ops(std::string_view name) const
    {
        const Entry *e = find(name);
        return e ? IdsT::fromOps(e->ops, e->op_count) : IdsT{};
    }

    /// The feature's GENERIC setParams op-id (FeatureParamsOperation.hpp), or
    /// kInvalidTypeId if it exposes no editable params. The settings panel pushes
    /// a reflected blob here via FeatureParamsProxy — works for any feature
    /// (including plugins) without knowing its concrete type.
    [[nodiscard]] TypeId paramSetOp(std::string_view name) const
    {
        const Entry *e = find(name);
        if (!e || e->param_set_op_index < 0 || e->param_set_op_index >= static_cast<int>(e->op_count))
        {
            return kInvalidTypeId;
        }
        return e->ops[e->param_set_op_index];
    }

    /// 声明的类型级元数据(依赖/冲突/多重性)。缺席 → 空描述符(type 无效)。
    [[nodiscard]] const FeatureDescriptor *descriptor(std::string_view name) const
    {
        const Entry *e = find(name);
        return e ? &e->registration.factory.descriptor : nullptr;
    }

    // ── 声明序遍历 + 稳定类型反查(attach 解析器的两条腿) ──

    [[nodiscard]] std::size_t size() const noexcept
    {
        return entries_.size();
    }
    [[nodiscard]] std::string_view nameAt(std::size_t i) const
    {
        return entries_[i].name;
    }
    [[nodiscard]] const FeatureDescriptor &descriptorAt(std::size_t i) const
    {
        return entries_[i].registration.factory.descriptor;
    }

    /// 稳定 FeatureTypeId(featureId("lux.render.xxx.v1"))→ 注册名。
    /// 依赖边以稳定 id 声明,而根集合与 attach 计划按名字说话 —— 这就是
    /// 两套词汇之间唯一的桥。缺席 → 空串。
    [[nodiscard]] std::string_view nameOfType(FeatureTypeId type) const
    {
        for (const auto &e : entries_)
        {
            if (e.registration.factory.descriptor.type == type)
            {
                return e.name;
            }
        }
        return {};
    }

    // Resolve only the explicitly selected features; registration order has no scene meaning.
    struct ResolveOutcome
    {
        // Stable topological order; ties retain the formal selection order.
        std::vector<std::string_view> order;
        /// 根名不在目录 —— 「声明了却没注册 TYPE」的旧 warning 等价物。
        std::vector<std::string_view> unknown;
        struct MissingDep
        {
            std::string_view dependent; ///< 谁声明的依赖
            FeatureTypeId dep;          ///< 缺的稳定 type id
        };
        std::vector<MissingDep> missing_deps;
        // Cycles reject the complete selection.
        std::vector<std::string_view> cycle;
        std::vector<MissingDep> conflicts;
        std::vector<MissingDep> version_mismatches;
        [[nodiscard]] bool valid() const noexcept
        {
            return unknown.empty() && missing_deps.empty() && cycle.empty() && conflicts.empty() &&
                version_mismatches.empty();
        }
    };

    [[nodiscard]] ResolveOutcome resolveAttachOrder(std::span<const std::string_view> roots) const
    {
        ResolveOutcome out;
        std::vector<std::size_t> selected;
        std::vector<bool> in_set(entries_.size(), false);
        for (const auto name : roots)
        {
            const auto index = indexOf(name);
            if (index == kNpos || in_set[index])
            {
                out.unknown.push_back(name);
                continue;
            }
            selected.push_back(index);
            in_set[index] = true;
        }
        for (const auto index : selected)
        {
            const auto& entry = entries_[index];
            for (const auto& dependency : entry.registration.factory.descriptor.dependencies)
            {
                const auto provider = indexOfType(dependency.type);
                const bool present = provider != kNpos && in_set[provider];
                if (!present && !dependency.optional)
                    out.missing_deps.push_back({entry.name, dependency.type});
                if (present && entries_[provider].registration.factory.descriptor.abi_version != dependency.abi_version)
                    out.version_mismatches.push_back({entry.name, dependency.type});
            }
            for (const auto conflict : entry.registration.factory.descriptor.conflicts)
            {
                const auto other = indexOfType(conflict);
                if (other != kNpos && in_set[other])
                    out.conflicts.push_back({entry.name, conflict});
            }
        }
        if (!out.valid())
            return out;
        std::vector<bool> placed(entries_.size(), false);
        while (out.order.size() < selected.size())
        {
            bool progressed = false;
            // Formal selection order breaks ties, independently of DLL admission order.
            for (const auto index : selected)
            {
                if (placed[index])
                    continue;
                const bool ready = std::ranges::all_of(entries_[index].registration.factory.descriptor.dependencies, [&](const auto& dep) {
                    const auto provider = indexOfType(dep.type);
                    return provider == kNpos || !in_set[provider] || placed[provider];
                });
                if (!ready)
                    continue;
                placed[index] = true;
                out.order.push_back(entries_[index].name);
                progressed = true;
                break;
            }
            if (!progressed)
            {
                for (const auto index : selected)
                    if (!placed[index]) out.cycle.push_back(entries_[index].name);
                out.order.clear();
                break;
            }
        }
        return out;
    }

    /// Test-only seam: register a feature entry with EXPLICIT dynamic op-ids,
    /// bypassing the server round-trip the normal add() needs (a live
    /// GeneralRenderServer + GPU device). A headless bridge test pairs this with
    /// a FrameDispatcher that registers recording handlers at these SAME TypeIds,
    /// so ops<IdsT>(name) resolves to ids the fake dispatcher actually handles.
    /// Tests that need scene handles acquire a RenderSceneLease separately:
    /// handles are scene-scoped state and never live in this Catalog.
    /// Never called on a production path.
    void injectForTest(std::string_view name, std::span<const TypeId> ops, int param_set_op_index = -1)
    {
        Entry e{};
        e.name = std::string(name);
        e.feature_type_id = 0;
        if (ops.size() > 16 || find(name)) std::terminate();
        e.op_count = static_cast<std::uint32_t>(ops.size());
        for (std::uint32_t i = 0; i < e.op_count; ++i)
        {
            e.ops[i] = ops[i];
        }
        e.param_set_op_index = param_set_op_index;
        entries_.push_back(std::move(e));
    }

  public:
    struct Entry
    {
        RenderFeatureRegistration registration;
        std::string name;
        std::uint32_t feature_type_id{0}; ///< 服务端动态注册序号
        TypeId ops[16]{};
        std::uint32_t op_count{0};
        int param_set_op_index{-1};
    };

    [[nodiscard]] const Entry *find(std::string_view name) const
    {
        for (const auto &e : entries_)
        {
            if (e.name == name)
            {
                return &e;
            }
        }
        return nullptr;
    }

    [[nodiscard]] const Entry *find(FeatureTypeId type) const noexcept
    {
        const auto index = indexOfType(type);
        return index == kNpos ? nullptr : &entries_[index];
    }

  private:
    static constexpr std::size_t kNpos = static_cast<std::size_t>(-1);

    [[nodiscard]] std::size_t indexOf(std::string_view name) const
    {
        for (std::size_t i = 0; i < entries_.size(); ++i)
        {
            if (entries_[i].name == name)
            {
                return i;
            }
        }
        return kNpos;
    }

    [[nodiscard]] std::size_t indexOfType(FeatureTypeId type) const
    {
        if (type == kInvalidFeatureTypeId)
        {
            return kNpos;
        }
        for (std::size_t i = 0; i < entries_.size(); ++i)
        {
            if (entries_[i].registration.factory.descriptor.type == type)
            {
                return i;
            }
        }
        return kNpos;
    }

    [[nodiscard]] static bool hasMissing(const ResolveOutcome &out, std::string_view dependent, FeatureTypeId dep)
    {
        for (const auto &m : out.missing_deps)
        {
            if (m.dependent == dependent && m.dep == dep)
            {
                return true;
            }
        }
        return false;
    }

    std::deque<Entry> entries_;
};

/// attach 目录的一个条目 —— **纯数据,无行为**。config 是精确
/// sizeof(CommConfig) 的字节(CommConfig 按契约可平凡拷贝,本来就是
/// memcpy 过 comm 通道的;服务端 decodeCommConfig 精确长度匹配,错长
/// 响亮失败)。发出走 `RenderProgramSession::addFeatureRaw`。
///
/// 此前这里是一个捕获 typed config 的 `std::function` thunk —— 类型擦除
/// 由字节承担之后,目录成为可遍历、可序列化的进程域数据(装配归属 ADR
/// 裁决四:plan 先天是数据,生成器只是可选优化)。
struct FeatureAttach
{
    std::string name; ///< == FeatureFactory.name(目录寻址键)
    /// 同一个 feature TYPE 的第二套**配置**(空 = 标准那套)。区分的是同名
    /// feature 的配置变体,不是「谁该有哪些 feature」;名字相同、profile
    /// 不同的条目互斥,宿主 attach 时指定自己要哪一套。
    std::string profile;
    std::uint32_t type_id{0};      ///< 服务端动态 type id
    std::vector<std::byte> config; ///< 精确 sizeof(CommConfig) 字节
};

/// Cold-assembly name -> FeatureHandle draft. Scene installers may use it
/// while materializing feature-owned integration state, then discard it;
/// process-lifetime topology remains in FeatureCatalog.
class FeatureBindings
{
  public:
    void bind(std::string_view name, FeatureHandle handle)
    {
        for (auto &e : entries_)
        {
            if (e.first == name)
            {
                e.second = handle;
                return;
            }
        }
        entries_.emplace_back(std::string(name), handle);
    }

    /// 缺席 → 无效句柄(消费方优雅 no-op —— 与 feature 缺席同一契约)。
    [[nodiscard]] FeatureHandle handle(std::string_view name) const noexcept
    {
        for (const auto &e : entries_)
        {
            if (e.first == name)
            {
                return e.second;
            }
        }
        return FeatureHandle{};
    }

  private:
    std::vector<std::pair<std::string, FeatureHandle>> entries_;
};

/// 两指针轻量视图 —— 子系统的取用面(`ctx.features()` 的返回型,按值传)。
/// 方法与旧的「带句柄的目录」同形,所以全部子系统调用点零改动;
/// 空指针 no-op 内建,不再需要静态空对象回退。
class RenderCapabilities
{
  public:
    RenderCapabilities() = default;
    RenderCapabilities(const FeatureCatalog *catalog, const FeatureBindings *bindings) noexcept
        : catalog_(catalog), bindings_(bindings)
    {
    }

    template <class IdsT> [[nodiscard]] IdsT ops(std::string_view name) const
    {
        return catalog_ ? catalog_->ops<IdsT>(name) : IdsT{};
    }

    [[nodiscard]] FeatureHandle handle(std::string_view name) const noexcept
    {
        return bindings_ ? bindings_->handle(name) : FeatureHandle{};
    }

    [[nodiscard]] TypeId paramSetOp(std::string_view name) const
    {
        return catalog_ ? catalog_->paramSetOp(name) : kInvalidTypeId;
    }

    [[nodiscard]] std::uint32_t typeId(std::string_view name) const
    {
        return catalog_ ? catalog_->typeId(name) : 0u;
    }

  private:
    const FeatureCatalog *catalog_{nullptr};
    const FeatureBindings *bindings_{nullptr};
};

} // namespace lux::render
