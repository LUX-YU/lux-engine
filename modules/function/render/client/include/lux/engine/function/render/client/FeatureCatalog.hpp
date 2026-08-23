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

#include <lux/engine/function/render/client/protocol/FeatureFactory.hpp>   // FeatureFactory / FeatureDescriptor / TypeId
#include <lux/engine/function/render/client/core/FeatureHandle.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
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
        /// add() order == attach 并列决胜 order(resolver 的稳定拓扑排序以
        /// 声明序断平局 —— 同 stage 写后写的叠放语义靠它,例如 Grid2D 在
        /// Canvas2D 之后)。
        void add(const FeatureFactory& factory,
                 std::uint32_t server_type_id,
                 std::span<const TypeId> ops)
        {
            if (factory.name == nullptr || factory.name[0] == '\0')
                return;
            Entry e{};
            e.name            = factory.name;
            e.feature_type_id = server_type_id;
            e.op_count        = static_cast<std::uint32_t>(std::min<std::size_t>(ops.size(), 16));
            for (std::uint32_t i = 0; i < e.op_count; ++i)
                e.ops[i] = ops[i];
            e.param_set_op_index = factory.param_set_op_index;
            e.descriptor         = factory.descriptor;   // 平凡拷贝;span 指向 render 模块静态存储
            entries_.push_back(std::move(e));
        }

        // ── Lookups by name (== RenderFeature::name()) ──

        /// The server-registered feature TYPE id (0 if absent). Type ids are
        /// process-scoped; per-scene attach (client addFeature) addresses types
        /// by this id — the returned per-scene handle lands in FeatureBindings.
        [[nodiscard]] std::uint32_t typeId(std::string_view name) const
        {
            const Entry* e = find(name);
            return e ? e->feature_type_id : 0u;
        }

        /// Typed op-ids — e.g. ops<LightOperationIds>("Light"). Returns IdsT{}
        /// (invalid) when the feature is absent, so the matching XxxProxy no-ops.
        template <class IdsT>
        [[nodiscard]] IdsT ops(std::string_view name) const
        {
            const Entry* e = find(name);
            return e ? IdsT::fromOps(e->ops, e->op_count) : IdsT{};
        }

        /// The feature's GENERIC setParams op-id (FeatureParamsOperation.hpp), or
        /// kInvalidTypeId if it exposes no editable params. The settings panel pushes
        /// a reflected blob here via FeatureParamsProxy — works for any feature
        /// (including plugins) without knowing its concrete type.
        [[nodiscard]] TypeId paramSetOp(std::string_view name) const
        {
            const Entry* e = find(name);
            if (!e || e->param_set_op_index < 0 ||
                e->param_set_op_index >= static_cast<int>(e->op_count))
                return kInvalidTypeId;
            return e->ops[e->param_set_op_index];
        }

        /// 声明的类型级元数据(依赖/冲突/多重性)。缺席 → 空描述符(type 无效)。
        [[nodiscard]] const FeatureDescriptor* descriptor(std::string_view name) const
        {
            const Entry* e = find(name);
            return e ? &e->descriptor : nullptr;
        }

        // ── 声明序遍历 + 稳定类型反查(attach 解析器的两条腿) ──

        [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
        [[nodiscard]] std::string_view nameAt(std::size_t i) const { return entries_[i].name; }
        [[nodiscard]] const FeatureDescriptor& descriptorAt(std::size_t i) const
        { return entries_[i].descriptor; }

        /// 稳定 FeatureTypeId(featureId("lux.render.xxx.v1"))→ 注册名。
        /// 依赖边以稳定 id 声明,而根集合与 attach 计划按名字说话 —— 这就是
        /// 两套词汇之间唯一的桥。缺席 → 空串。
        [[nodiscard]] std::string_view nameOfType(FeatureTypeId type) const
        {
            for (const auto& e : entries_)
                if (e.descriptor.type == type) return e.name;
            return {};
        }

        // ── attach 解析器:根 + 闭包 + 稳定拓扑排序(装配归属 ADR 裁决一) ──

        /// resolveAttachOrder 的结构化结果。render 模块受 no_terminal_io 门禁,
        /// 解析器**只记账不打印** —— unknown / missing_deps / cycle 的上报出口
        /// 在宿主(SceneRuntime),文字由它定。
        struct ResolveOutcome
        {
            /// attach 顺序。string_view 指向目录条目的常驻存储(目录活得比
            /// 场景久)。必需依赖已闭包拉入;并列决胜 = 目录声明序 —— 根集合
            /// 不变时输出**严格等于**声明序过滤结果,同 stage 写后写的叠放
            /// 语义(Grid2D 在 Canvas2D 之后)因此保持。
            std::vector<std::string_view> order;
            /// 根名不在目录 —— 「声明了却没注册 TYPE」的旧 warning 等价物。
            std::vector<std::string_view> unknown;
            struct MissingDep
            {
                std::string_view dependent;   ///< 谁声明的依赖
                FeatureTypeId    dep;         ///< 缺的稳定 type id
            };
            /// 必需依赖的 TYPE 没注册进目录。被依赖方仍留在 order 里 ——
            /// attach 会在服务端四道闸上响亮失败,而不是这里静默跳过。
            std::vector<MissingDep> missing_deps;
            /// 卷入依赖环的条目(不该发生;发生时回退声明序附在 order 尾)。
            std::vector<std::string_view> cycle;
        };

        /// 根集合(子系统声明并集 ∪ profile 管线根)→ attach 集合与顺序:
        /// 对 descriptor 的必需依赖做闭包,再做**稳定**拓扑排序(每轮取声明序
        /// 最靠前且依赖已就位者)。可选依赖(FeatureDependency::optional)不拉
        /// 闭包,但双方都在集合里时参与排序(DeferredLighting→ShadowMap 的
        /// 「在场必须排前」正是这形状)。纯函数,不发命令。
        [[nodiscard]] ResolveOutcome resolveAttachOrder(
            std::span<const std::string_view> roots) const
        {
            ResolveOutcome out;

            // 根入集(按目录条目下标),不在目录的根记账。
            std::vector<bool> in_set(entries_.size(), false);
            for (const auto r : roots)
            {
                const std::size_t i = indexOf(r);
                if (i == kNpos) out.unknown.push_back(r);
                else            in_set[i] = true;
            }

            // 必需依赖闭包(BFS;目录 ≤ 几十条,线性扫即可)。
            for (bool grew = true; grew; )
            {
                grew = false;
                for (std::size_t i = 0; i < entries_.size(); ++i)
                {
                    if (!in_set[i]) continue;
                    for (const FeatureDependency& d : entries_[i].descriptor.dependencies)
                    {
                        if (d.optional) continue;
                        const std::size_t j = indexOfType(d.type);
                        if (j == kNpos)
                        {
                            if (!hasMissing(out, entries_[i].name, d.type))
                                out.missing_deps.push_back({entries_[i].name, d.type});
                            continue;
                        }
                        if (!in_set[j]) { in_set[j] = true; grew = true; }
                    }
                }
            }

            // Kahn(稳定):每轮取声明序最靠前、且集合内依赖(必需 + 可选)
            // 都已落位的条目。无进展 ⇒ 剩余卷入环,按声明序附尾。
            std::vector<bool> placed(entries_.size(), false);
            for (;;)
            {
                bool progressed = false;
                for (std::size_t i = 0; i < entries_.size(); ++i)
                {
                    if (!in_set[i] || placed[i]) continue;
                    bool ready = true;
                    for (const FeatureDependency& d : entries_[i].descriptor.dependencies)
                    {
                        const std::size_t j = indexOfType(d.type);
                        if (j == kNpos || !in_set[j]) continue;   // 缺席的可选/缺注册的必需:不挡
                        if (!placed[j]) { ready = false; break; }
                    }
                    if (!ready) continue;
                    out.order.push_back(entries_[i].name);
                    placed[i]  = true;
                    progressed = true;
                }
                bool remaining = false;
                for (std::size_t i = 0; i < entries_.size(); ++i)
                    if (in_set[i] && !placed[i]) { remaining = true; break; }
                if (!remaining) break;
                if (!progressed)
                {
                    for (std::size_t i = 0; i < entries_.size(); ++i)
                        if (in_set[i] && !placed[i])
                        {
                            out.cycle.push_back(entries_[i].name);
                            out.order.push_back(entries_[i].name);
                            placed[i] = true;
                        }
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
        /// Tests that need scene handles seal a SceneRenderBinding separately:
        /// handles are scene-scoped state and never live in this Catalog.
        /// Never called on a production path.
        void injectForTest(std::string_view name,
                           std::span<const TypeId> ops,
                           int param_set_op_index = -1)
        {
            Entry e{};
            e.name               = std::string(name);
            e.feature_type_id    = 0;
            e.op_count           = static_cast<std::uint32_t>(std::min<std::size_t>(ops.size(), 16));
            for (std::uint32_t i = 0; i < e.op_count; ++i)
                e.ops[i] = ops[i];
            e.param_set_op_index = param_set_op_index;
            entries_.push_back(std::move(e));
        }

    private:
        struct Entry
        {
            std::string       name;
            std::uint32_t     feature_type_id{0};   ///< 服务端动态注册序号
            TypeId            ops[16]{};
            std::uint32_t     op_count{0};
            int               param_set_op_index{-1};
            FeatureDescriptor descriptor{};          ///< 稳定 type + 依赖/冲突/多重性
        };

        [[nodiscard]] const Entry* find(std::string_view name) const
        {
            for (const auto& e : entries_)
                if (e.name == name) return &e;
            return nullptr;
        }

        static constexpr std::size_t kNpos = static_cast<std::size_t>(-1);

        [[nodiscard]] std::size_t indexOf(std::string_view name) const
        {
            for (std::size_t i = 0; i < entries_.size(); ++i)
                if (entries_[i].name == name) return i;
            return kNpos;
        }

        [[nodiscard]] std::size_t indexOfType(FeatureTypeId type) const
        {
            if (type == kInvalidFeatureTypeId) return kNpos;
            for (std::size_t i = 0; i < entries_.size(); ++i)
                if (entries_[i].descriptor.type == type) return i;
            return kNpos;
        }

        [[nodiscard]] static bool hasMissing(const ResolveOutcome& out,
                                             std::string_view dependent,
                                             FeatureTypeId dep)
        {
            for (const auto& m : out.missing_deps)
                if (m.dependent == dependent && m.dep == dep) return true;
            return false;
        }

        std::vector<Entry> entries_;
    };

    /// attach 目录的一个条目 —— **纯数据,无行为**。config 是精确
    /// sizeof(CommConfig) 的字节(CommConfig 按契约可平凡拷贝,本来就是
    /// memcpy 过 comm 通道的;服务端 decodeCommConfig 精确长度匹配,错长
    /// 响亮失败)。发出走 `RenderFrameSession::addFeatureRaw`。
    ///
    /// 此前这里是一个捕获 typed config 的 `std::function` thunk —— 类型擦除
    /// 由字节承担之后,目录成为可遍历、可序列化的进程域数据(装配归属 ADR
    /// 裁决四:plan 先天是数据,生成器只是可选优化)。
    struct FeatureAttach
    {
        std::string            name;      ///< == FeatureFactory.name(目录寻址键)
        /// 同一个 feature TYPE 的第二套**配置**(空 = 标准那套)。区分的是同名
        /// feature 的配置变体,不是「谁该有哪些 feature」;名字相同、profile
        /// 不同的条目互斥,宿主 attach 时指定自己要哪一套。
        std::string            profile;
        std::uint32_t          type_id{0};   ///< 服务端动态 type id
        std::vector<std::byte> config;       ///< 精确 sizeof(CommConfig) 字节
    };

    /// Cold-assembly name → FeatureHandle draft. SceneRenderBinding receives
    /// it once in seal(); published Systems see only the resulting const
    /// RenderCapabilities view, so topology has no live mutation surface.
    class FeatureBindings
    {
    public:
        void bind(std::string_view name, FeatureHandle handle)
        {
            for (auto& e : entries_)
                if (e.first == name) { e.second = handle; return; }
            entries_.emplace_back(std::string(name), handle);
        }

        /// 缺席 → 无效句柄(消费方优雅 no-op —— 与 feature 缺席同一契约)。
        [[nodiscard]] FeatureHandle handle(std::string_view name) const noexcept
        {
            for (const auto& e : entries_)
                if (e.first == name) return e.second;
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
        RenderCapabilities(const FeatureCatalog*  catalog,
                      const FeatureBindings* bindings) noexcept
            : catalog_(catalog), bindings_(bindings) {}

        template <class IdsT>
        [[nodiscard]] IdsT ops(std::string_view name) const
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
        const FeatureCatalog*  catalog_{nullptr};
        const FeatureBindings* bindings_{nullptr};
    };

} // namespace lux::render
