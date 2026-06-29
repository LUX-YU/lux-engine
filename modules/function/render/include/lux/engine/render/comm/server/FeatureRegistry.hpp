#pragma once
// ============================================================================
//  FeatureRegistry.hpp — name-keyed feature registration + op-id store.
//
//  Replaces the editor's hard-coded FIP indices + per-feature op-id fields:
//  register an ORDERED set of factories (built-in OR plugin), create the scene
//  from them, then look up each feature's per-scene handle + dynamic op-ids BY
//  NAME (== FeatureFactory.name == RenderFeature::name()). A plugin just add()s
//  its own factory — the editor discovers + addresses it with ZERO compile-time
//  coupling (the name travels with the factory, the ops are dynamic).
// ============================================================================

#include <lux/engine/render/comm/server/RenderServer.hpp>   // GeneralRenderServer, FeatureInitParam, FeatureFactory
#include <lux/engine/render/core/FeatureHandle.hpp>

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
    class FeatureRegistry
    {
    public:
        using GeneralServer = GeneralRenderServer;

        /// Register @p factory on the server + record its name / type-id / ops.
        /// @p cfg / @p cfg_size are the per-scene FeatureInitParam payload (null/0
        /// for config-less features). add() order == createScene attach order —
        /// order matters only for attach-time resource dependencies (e.g. Light
        /// before Shadow), NOT for RG pass order (the graph resolves that by data
        /// dependency).
        void add(GeneralServer& server, const FeatureFactory& factory,
                 const void* cfg = nullptr, std::size_t cfg_size = 0)
        {
            const auto reply = server.addFeatureFactory(factory);
            Entry e{};
            e.name            = factory.name ? factory.name : "";
            e.feature_type_id = reply.feature_type_id;
            e.op_count        = reply.op_count;
            for (std::uint32_t i = 0; i < reply.op_count && i < 16u; ++i)
                e.ops[i] = reply.ops[i];
            e.cfg                = cfg;
            e.cfg_size           = cfg_size;
            e.param_set_op_index = factory.param_set_op_index;
            entries_.push_back(std::move(e));
        }

        /// FeatureInitParam list (in add() order) for GeneralRenderServer::createScene.
        [[nodiscard]] std::vector<GeneralServer::FeatureInitParam> initParams() const
        {
            std::vector<GeneralServer::FeatureInitParam> fips;
            fips.reserve(entries_.size());
            for (const auto& e : entries_)
                fips.push_back({e.feature_type_id, e.cfg, e.cfg_size});
            return fips;
        }

        /// After createScene: bind the returned per-scene handles (same order as add()).
        void bindHandles(std::span<const FeatureHandle> handles)
        {
            const std::size_t n = std::min(handles.size(), entries_.size());
            for (std::size_t i = 0; i < n; ++i)
                entries_[i].handle = handles[i];
        }

        // ── Lookups by name (== RenderFeature::name()) ──
        [[nodiscard]] bool has(std::string_view name) const { return find(name) != nullptr; }

        [[nodiscard]] FeatureHandle handle(std::string_view name) const
        {
            const Entry* e = find(name);
            return e ? e->handle : FeatureHandle{};
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

        /// All registered feature names (for diagnostics / iteration).
        [[nodiscard]] std::vector<std::string_view> names() const
        {
            std::vector<std::string_view> out;
            out.reserve(entries_.size());
            for (const auto& e : entries_) out.push_back(e.name);
            return out;
        }

    private:
        struct Entry
        {
            std::string   name;
            std::uint32_t feature_type_id{0};
            TypeId        ops[16]{};
            std::uint32_t op_count{0};
            const void*   cfg{nullptr};
            std::size_t   cfg_size{0};
            int           param_set_op_index{-1};
            FeatureHandle handle{};
        };

        [[nodiscard]] const Entry* find(std::string_view name) const
        {
            for (const auto& e : entries_)
                if (e.name == name) return &e;
            return nullptr;
        }

        std::vector<Entry> entries_;
    };

} // namespace lux::render
