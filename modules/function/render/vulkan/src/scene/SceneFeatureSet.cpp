#include <lux/engine/render/scene/SceneFeatureSet.hpp>
#include <lux/engine/render/RenderFeature.hpp>   // static_cast 回 RenderFeature* 需完整类型

#include <utility>

namespace lux::render
{
    // ─────────────────────────────────────────────────────────────────────
    //  容器
    // ─────────────────────────────────────────────────────────────────────

    FeatureHandle SceneFeatureSet::insertSlot(std::unique_ptr<SceneFeature> feature,
                                              PassContributeFn contribute)
    {
        // 只插入并返回句柄 —— feature->feature_id_ 是私有的,由持 friend 权限的
        // RenderScene 回填。
        markCacheDirty();
        return features_.insert(Slot{std::move(feature), contribute});
    }

    bool SceneFeatureSet::erase(FeatureHandle handle)
    {
        if (!features_.contains(handle))
            return false;
        features_.erase(handle);
        markCacheDirty();
        return true;
    }

    void SceneFeatureSet::clear()
    {
        features_.clear();
        all_dense_.clear();
        enabled_dense_.clear();
        cache_dirty_ = false;
    }

    SceneFeature* SceneFeatureSet::get(FeatureHandle handle) const
    {
        // tryGet 同时挡掉未知句柄与陈旧句柄(代数不匹配)。
        auto* slot = features_.tryGet(handle);
        return slot ? slot->feature.get() : nullptr;
    }

    bool SceneFeatureSet::contains(FeatureHandle handle) const noexcept
    {
        return features_.contains(handle);
    }

    std::size_t SceneFeatureSet::size() const noexcept
    {
        return features_.size();
    }

    // ─────────────────────────────────────────────────────────────────────
    //  查询(全部经公开访问器 —— 读不需要 friend)
    // ─────────────────────────────────────────────────────────────────────

    bool SceneFeatureSet::hasType(FeatureTypeId type) const noexcept
    {
        if (type == kInvalidFeatureTypeId)
            return false;
        for (const auto& slot : features_.values())
            if (slot.feature && slot.feature->typeId() == type)
                return true;
        return false;
    }

    SceneFeature* SceneFeatureSet::findByName(std::string_view name) const noexcept
    {
        for (const auto& slot : features_.values())
            if (slot.feature && slot.feature->name() == name)
                return slot.feature.get();
        return nullptr;
    }

    const SceneFeature* SceneFeatureSet::firstRequiring(
        FeatureTypeId type, const SceneFeature* exclude) const noexcept
    {
        if (type == kInvalidFeatureTypeId)
            return nullptr;
        for (const auto& other_slot : features_.values())
        {
            if (!other_slot.feature || other_slot.feature.get() == exclude)
                continue;
            for (const auto& dep : other_slot.feature->descriptor().dependencies)
                if (!dep.optional && dep.type == type)
                    return other_slot.feature.get();
        }
        return nullptr;
    }

    std::vector<SceneFeatureSet::FeatureParamDesc> SceneFeatureSet::queryParamDescs() const
    {
        std::vector<FeatureParamDesc> descs;
        descs.reserve(features_.size());
        for (const auto& slot : features_.values())
        {
            const auto* f = slot.feature.get();
            if (!f) continue;
            const std::string_view sn = f->paramStructName();
            const bool  has_params = !sn.empty();
            const void* data       = has_params ? const_cast<SceneFeature*>(f)->paramData() : nullptr;
            // data/size 一致性:空指针一律报 size 0,免得打包枚举流声称了它拿不出的字节。
            const std::size_t size = (has_params && data) ? f->paramSize() : 0;
            descs.push_back(FeatureParamDesc{
                f->featureId(),               // 完整句柄(5-5)
                f->isEnabled(),
                f->name(),
                sn,
                data,
                size});
        }
        return descs;
    }

    // ─────────────────────────────────────────────────────────────────────
    //  启用集稠密缓存
    // ─────────────────────────────────────────────────────────────────────

    void SceneFeatureSet::rebuildEnabledCacheIfNeeded() const
    {
        if (!cache_dirty_) return;

        // 一次遍历产出两份稠密视图,互为子集、顺序一致。它们只在这里被写。
        all_dense_.clear();
        enabled_dense_.clear();
        all_dense_.reserve(features_.size());
        enabled_dense_.reserve(features_.size());

        for (const auto& slot : features_.values())
        {
            auto* f = slot.feature.get();
            if (!f) continue;
            all_dense_.push_back(f);
            if (f->isEnabled())
                enabled_dense_.push_back(f);
        }
        cache_dirty_ = false;
    }

    std::span<SceneFeature* const> SceneFeatureSet::all() const
    {
        rebuildEnabledCacheIfNeeded();
        return {all_dense_.data(), all_dense_.size()};
    }

    std::span<SceneFeature* const> SceneFeatureSet::enabled() const
    {
        rebuildEnabledCacheIfNeeded();
        return {enabled_dense_.data(), enabled_dense_.size()};
    }

    void SceneFeatureSet::contributePasses(RGBuilder& builder) const
    {
        // 直接遍历槽,不经稠密缓存:图编译很稀疏(仅在图失效时),没必要为它
        // 多维护一份数组。顺序即装载顺序。
        for (const auto& slot : features_.values())
        {
            if (!slot.contribute || !slot.feature || !slot.feature->isEnabled())
                continue;
            slot.contribute(*slot.feature, builder);
        }
    }

    // ─────────────────────────────────────────────────────────────────────
    //  每(特性,视图)状态账本 —— 纯记账,不调任何特性虚函数
    // ─────────────────────────────────────────────────────────────────────

    bool SceneFeatureSet::ownsViewState(uint32_t feature_index, uint32_t view_index) const noexcept
    {
        auto it = view_states_.find(feature_index);
        return it != view_states_.end() && it->second.contains(view_index);
    }

    void SceneFeatureSet::recordViewState(uint32_t feature_index, uint32_t view_index)
    {
        view_states_[feature_index].insert(view_index);
    }

    void SceneFeatureSet::forgetViewState(uint32_t feature_index, uint32_t view_index) noexcept
    {
        auto it = view_states_.find(feature_index);
        if (it == view_states_.end())
            return;
        it->second.erase(view_index);
    }

    std::vector<uint32_t> SceneFeatureSet::viewsOwnedBy(uint32_t feature_index) const
    {
        std::vector<uint32_t> out;
        auto it = view_states_.find(feature_index);
        if (it == view_states_.end())
            return out;
        out.assign(it->second.begin(), it->second.end());
        return out;   // 刻意不改动账本 —— 见头文件说明
    }

    void SceneFeatureSet::forgetAllViewState(uint32_t feature_index) noexcept
    {
        view_states_.erase(feature_index);
    }

    void SceneFeatureSet::clearLedger() noexcept
    {
        view_states_.clear();
    }

} // namespace lux::render
