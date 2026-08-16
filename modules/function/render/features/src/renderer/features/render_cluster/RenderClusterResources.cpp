#include <lux/engine/render/renderer/features/render_cluster/RenderClusterResources.hpp>
#include <lux/engine/render/gpu/memory/GPUBuffer.hpp>
#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <lux/engine/render/gpu/lifecycle/DeferredDestroyQueue.hpp>
#include <lux/engine/render/resources/mesh/MeshCullCandidateSource.hpp>
#include <lux/engine/render/resources/mesh/InstanceResources.hpp>

#include <cstring>
#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace lux::render
{
    RenderClusterResources::CpuMemorySnapshot
    RenderClusterResources::cpuMemorySnapshot() const noexcept
    {
        CpuMemorySnapshot result{};
        const auto addAllocation = [&result](std::uint64_t bytes) noexcept
        {
            if (bytes == 0u)
                return;
            result.capacity_bytes += bytes;
            ++result.allocation_count;
        };
        const auto addString = [&addAllocation](const std::string& value)
        {
            addAllocation(value.capacity());
        };
        const auto addVector = [&addAllocation](const auto& values)
        {
            using Value = typename std::remove_cvref_t<
                decltype(values)>::value_type;
            addAllocation(values.capacity() * sizeof(Value));
        };
        const auto addMapStorage = [&addAllocation](const auto& values)
        {
            using Map = std::remove_cvref_t<decltype(values)>;
            addAllocation(values.bucket_count() * sizeof(void*));
            if (!values.empty())
            {
                addAllocation(values.size() * sizeof(
                    typename Map::value_type));
            }
        };
        const auto addSetStorage = [&addAllocation](const auto& values)
        {
            using Set = std::remove_cvref_t<decltype(values)>;
            addAllocation(values.bucket_count() * sizeof(void*));
            if (!values.empty())
            {
                addAllocation(values.size() * sizeof(
                    typename Set::value_type));
            }
        };

        addMapStorage(clusters_);
        for (const auto& [cluster_key, cluster] : clusters_)
        {
            addString(cluster_key);
            addVector(cluster.instances);
            addVector(cluster.objects);
            addVector(cluster.pick_tokens);
        }
        addMapStorage(latest_revision_);
        for (const auto& [revision_key, _] : latest_revision_)
            addString(revision_key);
        addMapStorage(parent_members_);
        for (const auto& [parent_key, members] : parent_members_)
        {
            addString(parent_key);
            addSetStorage(members);
            for (const auto& member : members)
                addString(member);
        }
        addSetStorage(hierarchy_parents_);
        for (const auto& parent : hierarchy_parents_)
            addString(parent);
        addMapStorage(hierarchy_prefer_children_);
        for (const auto& [parent_key, _] : hierarchy_prefer_children_)
            addString(parent_key);
        addVector(pick_gpu_slots_);
        addVector(gpu_cull_frames_);
        addVector(gpu_cull_clusters_);
        addVector(gpu_cull_instances_);
        addMapStorage(pick_ids_);
        addVector(free_pick_tokens_);
        addVector(retired_pick_tokens_);
        return result;
    }

    RenderClusterResources::~RenderClusterResources()
    {
        shutdownGpuCulling();
        shutdownPicking();
    }

    bool RenderClusterResources::canRebaseSceneOrigin(
        const std::int64_t origin_delta[3]) const noexcept
    {
        for (const auto& [_, cluster] : clusters_)
        {
            if (!canRebaseRenderPageDelta(
                    cluster.header.bounds_center.page_delta,
                    origin_delta))
            {
                return false;
            }
            for (const auto& instance : cluster.instances)
            {
                if (!canRebaseRenderPageDelta(
                        instance.transform.page_delta,
                        origin_delta))
                {
                    return false;
                }
            }
        }
        return true;
    }

    void RenderClusterResources::rebaseSceneOrigin(
        const std::int64_t origin_delta[3]) noexcept
    {
        for (auto& [_, cluster] : clusters_)
        {
            rebaseRenderPageDelta(
                cluster.header.bounds_center.page_delta,
                origin_delta);
            for (auto& instance : cluster.instances)
            {
                rebaseRenderPageDelta(
                    instance.transform.page_delta,
                    origin_delta);
            }
        }
        gpu_cull_dirty_ = true;
    }

    std::string RenderClusterResources::key(RenderClusterWireId id)
    {
        constexpr char digits[] = "0123456789abcdef";
        std::string result;
        result.resize(32u);
        for (std::size_t index = 0u; index < 16u; ++index)
        {
            result[index * 2u] = digits[id.bytes[index] >> 4u];
            result[index * 2u + 1u] = digits[id.bytes[index] & 15u];
        }
        return result;
    }

    bool RenderClusterResources::validatesUpsert(
        const UploadRenderClusterPayload& header,
        std::span<const RenderClusterWireInstance> instances,
        std::size_t object_count,
        std::size_t pick_token_count) const
    {
        std::unordered_set<std::string> child_ids;
        bool hierarchy_valid = header.child_count <=
            kMaximumRenderClusterChildren &&
            (!header.parent.valid() || header.parent != header.id);
        for (std::uint8_t index = 0u; index < header.child_count; ++index)
        {
            hierarchy_valid = hierarchy_valid &&
                header.children[index].valid() &&
                header.children[index] != header.id &&
                child_ids.insert(key(header.children[index])).second;
        }
        if (header.scene_id.isNull() || !header.id.valid() ||
            header.revision == 0u ||
            header.instance_count != instances.size() ||
            (object_count != 0u && object_count != instances.size()) ||
            (pick_token_count != 0u &&
                pick_token_count != instances.size()) ||
            header.instance_count == 0u ||
            !std::isfinite(header.bounds_radius) ||
            header.bounds_radius < 0.0f ||
            !std::isfinite(header.lod_error) ||
            !std::isfinite(header.hlod_enter_error_pixels) ||
            !std::isfinite(header.hlod_exit_error_pixels) ||
            header.hlod_enter_error_pixels <=
                header.hlod_exit_error_pixels ||
            header.hlod_exit_error_pixels < 0.0f ||
            !hierarchy_valid)
        {
            return false;
        }
        return true;
    }

    bool RenderClusterResources::upsert(
        const UploadRenderClusterPayload& header,
        std::span<const RenderClusterWireInstance> instances,
        std::vector<RenderObjectHandle> objects,
        std::vector<std::uint32_t> pick_tokens)
    {
        if (!validatesUpsert(
                header,
                instances,
                objects.size(),
                pick_tokens.size()))
        {
            return false;
        }
        const auto cluster_key = key(header.id);
        auto& latest = latest_revision_[cluster_key];
        if (header.revision <= latest)
            return true;
        latest = header.revision;
        transition_duration_seconds_ =
            static_cast<float>(header.transition_milliseconds) / 1000.0f;
        hlod_enter_error_pixels_ = header.hlod_enter_error_pixels;
        hlod_exit_error_pixels_ = header.hlod_exit_error_pixels;
        auto found = clusters_.find(cluster_key);
        if (found != clusters_.end())
        {
            retirePickTokens(found->second.pick_tokens);
            instance_count_ -= found->second.instances.size();
            if (found->second.visible)
            {
                --visible_cluster_count_;
                visible_instance_count_ -= found->second.instances.size();
            }
            if (found->second.header.parent.valid())
            {
                auto parent = parent_members_.find(
                    key(found->second.header.parent));
                if (parent != parent_members_.end())
                {
                    parent->second.erase(cluster_key);
                    if (parent->second.empty())
                        parent_members_.erase(parent);
                }
            }
            if (found->second.header.child_count != 0u)
            {
                hierarchy_parents_.erase(cluster_key);
                hierarchy_prefer_children_.erase(cluster_key);
            }
        }
        Cluster cluster;
        cluster.header = header;
        cluster.header.instances = {};
        cluster.instances.assign(instances.begin(), instances.end());
        cluster.objects = std::move(objects);
        cluster.pick_tokens = std::move(pick_tokens);
        if (header.parent.valid())
            parent_members_[key(header.parent)].insert(cluster_key);
        if (header.child_count != 0u)
        {
            hierarchy_parents_.insert(cluster_key);
            hierarchy_prefer_children_.try_emplace(cluster_key, true);
        }
        instance_count_ += cluster.instances.size();
        clusters_.insert_or_assign(cluster_key, std::move(cluster));
        gpu_cull_dirty_ = true;
        return true;
    }

    bool RenderClusterResources::accepts(
        RenderClusterWireId id,
        std::uint64_t revision) const noexcept
    {
        if (!id.valid() || revision == 0u)
            return false;
        const auto found = latest_revision_.find(key(id));
        return found == latest_revision_.end() || revision > found->second;
    }

    bool RenderClusterResources::remove(
        RenderClusterWireId id,
        std::uint64_t revision) noexcept
    {
        if (!id.valid() || revision == 0u)
            return false;
        const auto cluster_key = key(id);
        auto& latest = latest_revision_[cluster_key];
        if (revision <= latest)
            return true;
        latest = revision;
        const auto found = clusters_.find(cluster_key);
        if (found == clusters_.end())
            return true;
        if (found->second.header.parent.valid())
        {
            auto parent = parent_members_.find(
                key(found->second.header.parent));
            if (parent != parent_members_.end())
            {
                parent->second.erase(cluster_key);
                if (parent->second.empty())
                    parent_members_.erase(parent);
            }
        }
        if (found->second.header.child_count != 0u)
        {
            hierarchy_parents_.erase(cluster_key);
            hierarchy_prefer_children_.erase(cluster_key);
        }
        retirePickTokens(found->second.pick_tokens);
        instance_count_ -= found->second.instances.size();
        if (found->second.visible)
        {
            --visible_cluster_count_;
            visible_instance_count_ -= found->second.instances.size();
        }
        clusters_.erase(found);
        gpu_cull_dirty_ = true;
        return true;
    }

    std::vector<RenderClusterResources::VisibilityChange>
    RenderClusterResources::reconcileHierarchy(
        RenderClusterWireId family_parent,
        bool prefer_children,
        float scene_time,
        float transition_duration_seconds)
    {
        std::vector<VisibilityChange> changes;
        if (!family_parent.valid() || !std::isfinite(scene_time) ||
            !std::isfinite(transition_duration_seconds) ||
            transition_duration_seconds < 0.0f)
            return changes;
        const auto parent_key = key(family_parent);
        if (hierarchy_parents_.contains(parent_key))
            hierarchy_prefer_children_.insert_or_assign(
                parent_key, prefer_children);

        const auto family_seed = transitionSeed(
            0u, family_parent, 0u);
        const auto emit = [&changes, family_seed](
            Cluster& cluster,
            ETransitionAction transition)
        {
            changes.push_back(VisibilityChange{
                cluster.header.id,
                cluster.visible,
                transition,
                cluster.transition_start_time,
                cluster.transition_duration,
                family_seed});
        };
        const auto set_draw_visible = [this](
            Cluster& cluster,
            bool visible)
        {
            if (cluster.visible == visible)
                return;
            cluster.visible = visible;
            if (cluster.visible)
            {
                ++visible_cluster_count_;
                visible_instance_count_ += cluster.instances.size();
            }
            else
            {
                --visible_cluster_count_;
                visible_instance_count_ -= cluster.instances.size();
            }
        };
        const auto coverage_at = [scene_time](const Cluster& cluster)
        {
            if (cluster.visibility_state == EVisibilityState::VISIBLE)
                return 1.0f;
            if (cluster.visibility_state == EVisibilityState::HIDDEN)
                return 0.0f;
            const auto progress = std::clamp(
                (scene_time - cluster.transition_start_time) /
                    std::max(cluster.transition_duration, 1e-5f),
                0.0f,
                1.0f);
            return cluster.visibility_state == EVisibilityState::FADING_OUT
                ? 1.0f - progress
                : progress;
        };
        const auto settle = [
            scene_time,
            &emit,
            &set_draw_visible](Cluster& cluster)
        {
            if (cluster.visibility_state != EVisibilityState::FADING_IN &&
                cluster.visibility_state != EVisibilityState::FADING_OUT)
            {
                return;
            }
            if (scene_time < cluster.transition_start_time +
                    cluster.transition_duration)
            {
                return;
            }
            if (cluster.visibility_state == EVisibilityState::FADING_IN)
            {
                cluster.visibility_state = EVisibilityState::VISIBLE;
                emit(cluster, ETransitionAction::NONE);
            }
            else
            {
                cluster.visibility_state = EVisibilityState::HIDDEN;
                set_draw_visible(cluster, false);
                emit(cluster, ETransitionAction::NONE);
            }
        };
        const auto request_visibility = [
            scene_time,
            transition_duration_seconds,
            &coverage_at,
            &emit,
            &set_draw_visible,
            &settle](Cluster& cluster, bool visible)
        {
            settle(cluster);
            const auto already_requested = visible
                ? cluster.visibility_state == EVisibilityState::VISIBLE ||
                    cluster.visibility_state == EVisibilityState::FADING_IN
                : cluster.visibility_state == EVisibilityState::HIDDEN ||
                    cluster.visibility_state == EVisibilityState::FADING_OUT;
            if (already_requested)
                return;
            if (transition_duration_seconds <= 0.0f)
            {
                cluster.visibility_state = visible
                    ? EVisibilityState::VISIBLE
                    : EVisibilityState::HIDDEN;
                cluster.transition_start_time = scene_time;
                cluster.transition_duration = 0.0f;
                set_draw_visible(cluster, visible);
                emit(cluster, ETransitionAction::NONE);
                return;
            }

            const auto coverage = coverage_at(cluster);
            cluster.transition_duration = transition_duration_seconds;
            if (visible)
            {
                set_draw_visible(cluster, true);
                cluster.visibility_state = EVisibilityState::FADING_IN;
                cluster.transition_start_time = scene_time -
                    coverage * transition_duration_seconds;
                emit(cluster, ETransitionAction::FADE_IN);
            }
            else
            {
                cluster.visibility_state = EVisibilityState::FADING_OUT;
                cluster.transition_start_time = scene_time -
                    (1.0f - coverage) * transition_duration_seconds;
                emit(cluster, ETransitionAction::FADE_OUT);
            }
        };

        std::unordered_set<std::string> visited;
        std::unordered_set<std::string> visiting;
        std::function<void(const std::string&, bool)> reconcile_family;
        reconcile_family = [this,
                            &visited,
                            &visiting,
                            &request_visibility,
                            &reconcile_family](
                               const std::string& family_key,
                               bool family_enabled)
        {
            const auto parent = clusters_.find(family_key);
            if (parent == clusters_.end())
                return;
            if (!visiting.insert(family_key).second)
                return;

            auto& parent_cluster = parent->second;
            visited.insert(family_key);
            std::vector<std::string> children;
            children.reserve(parent_cluster.header.child_count);
            bool children_ready = family_enabled &&
                hierarchy_prefer_children_[family_key] &&
                parent_cluster.header.child_count != 0u;
            for (std::uint8_t index = 0u;
                 index < parent_cluster.header.child_count;
                 ++index)
            {
                const auto child_key = key(
                    parent_cluster.header.children[index]);
                children.push_back(child_key);
                const auto child = clusters_.find(child_key);
                children_ready = children_ready &&
                    child != clusters_.end() &&
                    child->second.header.parent ==
                        parent_cluster.header.id;
            }
            request_visibility(
                parent_cluster,
                family_enabled && !children_ready);
            for (const auto& child_key : children)
            {
                const auto child = clusters_.find(child_key);
                if (child == clusters_.end())
                    continue;
                if (hierarchy_parents_.contains(child_key))
                {
                    reconcile_family(
                        child_key,
                        family_enabled && children_ready);
                }
                else
                {
                    visited.insert(child_key);
                    request_visibility(
                        child->second,
                        family_enabled && children_ready);
                }
            }
            visiting.erase(family_key);
        };

        std::vector<std::string> roots;
        roots.reserve(clusters_.size());
        for (const auto& [cluster_key, cluster] : clusters_)
        {
            if (cluster.header.parent.valid() &&
                clusters_.contains(key(cluster.header.parent)))
            {
                continue;
            }
            roots.push_back(cluster_key);
        }
        std::ranges::sort(roots);
        for (const auto& root : roots)
        {
            auto found = clusters_.find(root);
            if (found == clusters_.end())
                continue;
            if (hierarchy_parents_.contains(root))
                reconcile_family(root, true);
            else
            {
                visited.insert(root);
                request_visibility(found->second, true);
            }
        }

        // A cycle is invalid cooked/runtime data. Keep every unreachable
        // representation hidden so it cannot produce double geometry while
        // the structured upload failure remains diagnosable by the owner.
        for (auto& [cluster_key, cluster] : clusters_)
            if (!visited.contains(cluster_key))
                request_visibility(cluster, false);

        if (!changes.empty())
            gpu_cull_dirty_ = true;
        return changes;
    }

    bool RenderClusterResources::prefersChildren(
        RenderClusterWireId family_parent) const noexcept
    {
        const auto found = hierarchy_prefer_children_.find(
            key(family_parent));
        return found != hierarchy_prefer_children_.end() && found->second;
    }

    std::size_t RenderClusterResources::transitionCount() const noexcept
    {
        return static_cast<std::size_t>(std::ranges::count_if(
            clusters_,
            [](const auto& entry)
            {
                return entry.second.visibility_state ==
                        EVisibilityState::FADING_IN ||
                    entry.second.visibility_state ==
                        EVisibilityState::FADING_OUT;
            }));
    }

    std::uint32_t RenderClusterResources::transitionSeed(
        std::uint64_t stable_pick_id,
        RenderClusterWireId cluster,
        std::size_t instance_index) noexcept
    {
        std::uint64_t value = stable_pick_id;
        if (value == 0u)
        {
            value = static_cast<std::uint64_t>(instance_index) +
                0x9e3779b97f4a7c15ull;
            for (const auto byte : cluster.bytes)
                value = (value ^ byte) * 0x100000001b3ull;
        }
        value ^= value >> 30u;
        value *= 0xbf58476d1ce4e5b9ull;
        value ^= value >> 27u;
        value *= 0x94d049bb133111ebull;
        value ^= value >> 31u;
        return static_cast<std::uint32_t>(value) ^
            static_cast<std::uint32_t>(value >> 32u);
    }

    std::vector<RenderClusterWireId>
    RenderClusterResources::hierarchyParents() const
    {
        std::vector<RenderClusterWireId> result;
        result.reserve(hierarchy_parents_.size() + transitionCount());
        std::unordered_set<std::string> included;
        for (const auto& parent_key : hierarchy_parents_)
        {
            const auto parent = clusters_.find(parent_key);
            if (parent != clusters_.end())
            {
                result.push_back(parent->second.header.id);
                included.insert(parent_key);
            }
        }
        // A standalone Cluster can fade in on first residency without being
        // an HLOD family parent. Keep it in the per-frame reconciliation set
        // until the timed state settles and its transition metadata is cleared.
        for (const auto& [cluster_key, cluster] : clusters_)
        {
            if (included.contains(cluster_key) ||
                (cluster.visibility_state != EVisibilityState::FADING_IN &&
                 cluster.visibility_state != EVisibilityState::FADING_OUT))
            {
                continue;
            }
            if (cluster.header.parent.valid() &&
                clusters_.contains(key(cluster.header.parent)))
            {
                continue;
            }
            result.push_back(cluster.header.id);
        }
        std::ranges::sort(
            result,
            [](const auto& left, const auto& right)
            {
                return key(left) < key(right);
            });
        return result;
    }

    const RenderClusterResources::Cluster* RenderClusterResources::find(
        RenderClusterWireId id) const
    {
        const auto found = clusters_.find(key(id));
        return found == clusters_.end() ? nullptr : &found->second;
    }

    void RenderClusterResources::forEachObject(
        const std::function<void(RenderObjectHandle)>& visitor) const
    {
        if (!visitor)
            return;
        for (const auto& [_, cluster] : clusters_)
            for (const auto object : cluster.objects)
                visitor(object);
    }

    void RenderClusterResources::forEachVisibleObject(
        const std::function<void(RenderObjectHandle)>& visitor) const
    {
        if (!visitor)
            return;
        for (const auto& [_, cluster] : clusters_)
        {
            if (!cluster.visible)
                continue;
            for (const auto object : cluster.objects)
                visitor(object);
        }
    }

    void RenderClusterResources::forEachVisiblePickObject(
        const std::function<void(
            RenderObjectHandle,
            std::uint32_t)>& visitor) const
    {
        if (!visitor)
            return;
        for (const auto& [_, cluster] : clusters_)
        {
            if (!cluster.visible)
                continue;
            const auto count = std::min(
                cluster.objects.size(), cluster.pick_tokens.size());
            for (std::size_t index = 0u; index < count; ++index)
            {
                if (cluster.pick_tokens[index] != 0u)
                {
                    visitor(
                        cluster.objects[index],
                        cluster.pick_tokens[index]);
                }
            }
        }
    }

    std::uint32_t RenderClusterResources::allocatePickToken(
        std::uint64_t stable_pick_id)
    {
        std::uint32_t token = 0u;
        if (!free_pick_tokens_.empty())
        {
            token = free_pick_tokens_.back();
            free_pick_tokens_.pop_back();
        }
        else if (next_pick_token_ <= kMaximumPickToken)
        {
            token = next_pick_token_++;
        }
        if (token != 0u)
            pick_ids_.insert_or_assign(token, stable_pick_id);
        return token;
    }

    void RenderClusterResources::cancelPickToken(
        std::uint32_t token) noexcept
    {
        if (token == 0u || pick_ids_.erase(token) != 1u)
            return;
        free_pick_tokens_.push_back(token);
    }

    std::optional<std::uint64_t>
    RenderClusterResources::resolvePickToken(
        std::uint32_t token) const noexcept
    {
        const auto found = pick_ids_.find(token);
        if (found == pick_ids_.end())
            return std::nullopt;
        return found->second;
    }

    void RenderClusterResources::retirePickTokens(
        std::span<const std::uint32_t> tokens) noexcept
    {
        const auto retire_serial = pick_frame_serial_ +
            std::max<std::size_t>(pick_gpu_slots_.size(), 1u) + 1u;
        for (const auto token : tokens)
        {
            if (token == 0u || !pick_ids_.contains(token))
                continue;
            retired_pick_tokens_.push_back(
                RetiredPickToken{token, retire_serial});
        }
    }

    bool RenderClusterResources::initializePicking(
        DeviceContext& device,
        DeferredDestroyQueue& deferred_destroy,
        std::uint32_t frames_in_flight)
    {
        if (!pick_gpu_slots_.empty())
            return true;
        if (pick_device_ != nullptr && pick_device_ != &device)
            return false;
        pick_device_ = &device;
        deferred_destroy_ = &deferred_destroy;
        pick_gpu_slots_.resize(std::max(frames_in_flight, 1u));
        for (auto& slot : pick_gpu_slots_)
        {
            VmaAllocation allocation{nullptr};
            if (!createGpuBufferVmaBuffer(
                    device.vmaAllocator(),
                    sizeof(std::uint32_t),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    true,
                    &slot.buffer,
                    &allocation,
                    &slot.mapped) ||
                slot.buffer == VK_NULL_HANDLE || slot.mapped == nullptr)
            {
                slot.allocation = allocation;
                shutdownPicking();
                return false;
            }
            slot.allocation = allocation;
            *static_cast<std::uint32_t*>(slot.mapped) =
                std::numeric_limits<std::uint32_t>::max();
            flushGpuBufferVmaAllocation(
                device.vmaAllocator(), allocation, 0u,
                sizeof(std::uint32_t));
        }
        return true;
    }

    void RenderClusterResources::shutdownPicking() noexcept
    {
        if (pick_device_ != nullptr)
        {
            for (auto& slot : pick_gpu_slots_)
            {
                if (slot.buffer != VK_NULL_HANDLE && deferred_destroy_)
                {
                    deferred_destroy_->retireBuffer(
                        slot.buffer,
                        static_cast<VmaAllocation>(slot.allocation));
                }
                else
                {
                    destroyGpuBufferVmaBuffer(
                        pick_device_->vmaAllocator(),
                        slot.buffer,
                        static_cast<VmaAllocation>(slot.allocation));
                }
                slot = {};
            }
        }
        pick_gpu_slots_.clear();
        if (gpu_cull_frames_.empty())
        {
            pick_device_ = nullptr;
            deferred_destroy_ = nullptr;
        }
        pending_pick_.reset();
    }

    bool RenderClusterResources::allocateGpuCullFrames(
        std::uint32_t frames_in_flight,
        std::uint32_t capacity)
    {
        if (!pick_device_ || capacity == 0u)
            return false;
        std::vector<GpuCullFrame> candidate(
            std::max(frames_in_flight, 1u));
        const auto cluster_bytes = static_cast<VkDeviceSize>(capacity) *
            sizeof(GpuCullCluster);
        const auto instance_bytes = static_cast<VkDeviceSize>(capacity) *
            sizeof(GpuCullInstance);
        for (auto& frame : candidate)
        {
            VmaAllocation cluster_allocation{nullptr};
            if (!createGpuBufferVmaBuffer(
                    pick_device_->vmaAllocator(),
                    cluster_bytes,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    true,
                    &frame.cluster_buffer,
                    &cluster_allocation,
                    &frame.cluster_mapped))
            {
                retireGpuCullFrames(candidate);
                return false;
            }
            frame.cluster_allocation = cluster_allocation;

            VmaAllocation instance_allocation{nullptr};
            if (!createGpuBufferVmaBuffer(
                    pick_device_->vmaAllocator(),
                    instance_bytes,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    true,
                    &frame.instance_buffer,
                    &instance_allocation,
                    &frame.instance_mapped))
            {
                frame.instance_allocation = instance_allocation;
                retireGpuCullFrames(candidate);
                return false;
            }
            frame.instance_allocation = instance_allocation;

            VmaAllocation dispatch_allocation{nullptr};
            if (!createGpuBufferVmaBuffer(
                    pick_device_->vmaAllocator(),
                    sizeof(CandidateDispatchState),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    true,
                    &frame.candidate_dispatch_buffer,
                    &dispatch_allocation,
                    &frame.candidate_dispatch_mapped))
            {
                frame.candidate_dispatch_allocation =
                    dispatch_allocation;
                retireGpuCullFrames(candidate);
                return false;
            }
            frame.candidate_dispatch_allocation = dispatch_allocation;
            std::memset(
                frame.candidate_dispatch_mapped,
                0,
                sizeof(CandidateDispatchState));
            flushGpuBufferVmaAllocation(
                pick_device_->vmaAllocator(),
                dispatch_allocation,
                0u,
                sizeof(CandidateDispatchState));
        }

        auto retired = std::move(gpu_cull_frames_);
        gpu_cull_frames_ = std::move(candidate);
        gpu_cull_capacity_ = capacity;
        retireGpuCullFrames(retired);
        return true;
    }

    void RenderClusterResources::retireGpuCullFrames(
        std::vector<GpuCullFrame>& frames) noexcept
    {
        if (!pick_device_)
        {
            frames.clear();
            return;
        }
        for (auto& frame : frames)
        {
            const auto retire = [this](VkBuffer buffer, void* allocation)
            {
                if (buffer == VK_NULL_HANDLE)
                    return;
                if (deferred_destroy_)
                {
                    deferred_destroy_->retireBuffer(
                        buffer,
                        static_cast<VmaAllocation>(allocation));
                }
                else
                {
                    destroyGpuBufferVmaBuffer(
                        pick_device_->vmaAllocator(),
                        buffer,
                        static_cast<VmaAllocation>(allocation));
                }
            };
            retire(frame.cluster_buffer, frame.cluster_allocation);
            retire(frame.instance_buffer, frame.instance_allocation);
            retire(
                frame.candidate_dispatch_buffer,
                frame.candidate_dispatch_allocation);
            frame = {};
        }
        frames.clear();
    }

    bool RenderClusterResources::initializeGpuCulling(
        DeviceContext& device,
        DeferredDestroyQueue& deferred_destroy,
        std::uint32_t frames_in_flight,
        std::uint32_t initial_capacity)
    {
        if (pick_device_ && pick_device_ != &device)
            return false;
        pick_device_ = &device;
        deferred_destroy_ = &deferred_destroy;
        if (!gpu_cull_frames_.empty())
            return true;
        return allocateGpuCullFrames(
            frames_in_flight,
            std::max(initial_capacity, 1u));
    }

    void RenderClusterResources::shutdownGpuCulling() noexcept
    {
        retireGpuCullFrames(gpu_cull_frames_);
        gpu_cull_clusters_.clear();
        gpu_cull_instances_.clear();
        gpu_cull_capacity_ = 0u;
        gpu_cull_revision_ = 1u;
        gpu_cull_instance_layout_serial_ = 0u;
        gpu_cull_dirty_ = true;
        latest_gpu_candidate_count_ = 0u;
        latest_gpu_candidate_requested_count_ = 0u;
        latest_gpu_candidate_overflow_count_ = 0u;
        latest_gpu_candidate_group_count_ = 0u;
        has_gpu_candidate_count_ = false;
        gpu_candidate_dispatch_valid_ = false;
        if (pick_gpu_slots_.empty())
        {
            pick_device_ = nullptr;
            deferred_destroy_ = nullptr;
        }
    }

    bool RenderClusterResources::rebuildGpuCullCanonical(
        const InstanceResources& instances)
    {
        gpu_cull_clusters_.clear();
        gpu_cull_instances_.clear();
        gpu_cull_clusters_.reserve(clusters_.size());
        gpu_cull_instances_.reserve(std::min<std::size_t>(
            instance_count_, gpu_cull_capacity_));
        for (const auto& [_, cluster] : clusters_)
        {
            if (gpu_cull_clusters_.size() >= gpu_cull_capacity_)
                return false;
            const auto cluster_index = static_cast<std::uint32_t>(
                gpu_cull_clusters_.size());
            GpuCullCluster gpu_cluster{};
            for (std::size_t axis = 0u; axis < 3u; ++axis)
            {
                gpu_cluster.page_visible[axis] =
                    cluster.header.bounds_center.page_delta[axis];
                gpu_cluster.local_radius[axis] =
                    cluster.header.bounds_center.local[axis];
            }
            gpu_cluster.page_visible[3] = cluster.visible ? 1 : 0;
            gpu_cluster.local_radius[3] = cluster.header.bounds_radius;
            gpu_cull_clusters_.push_back(gpu_cluster);

            for (const auto object : cluster.objects)
            {
                const auto slot = instances.resolveSlot(object);
                if (!instances.isAlive(slot))
                    continue;
                if (gpu_cull_instances_.size() >= gpu_cull_capacity_)
                    return false;
                gpu_cull_instances_.push_back(GpuCullInstance{
                    slot.index,
                    cluster_index});
            }
        }
        gpu_cull_instance_layout_serial_ = instances.slotLayoutSerial();
        gpu_cull_dirty_ = false;
        ++gpu_cull_revision_;
        if (gpu_cull_revision_ == 0u)
            gpu_cull_revision_ = 1u;
        return true;
    }

    bool RenderClusterResources::prepareGpuCulling(
        std::uint32_t frame_index,
        const InstanceResources& instances,
        bool& capacity_changed)
    {
        capacity_changed = false;
        if (!pick_device_ || gpu_cull_frames_.empty())
            return false;
        if (instances.capacity() > gpu_cull_capacity_)
        {
            if (!allocateGpuCullFrames(
                    static_cast<std::uint32_t>(gpu_cull_frames_.size()),
                    instances.capacity()))
            {
                return false;
            }
            capacity_changed = true;
        }

        if (gpu_cull_dirty_ ||
            gpu_cull_instance_layout_serial_ !=
                instances.slotLayoutSerial())
        {
            if (!rebuildGpuCullCanonical(instances))
                return false;
        }

        auto& frame = gpu_cull_frames_[
            frame_index % gpu_cull_frames_.size()];
        if (frame.candidate_submitted &&
            frame.candidate_dispatch_mapped != nullptr)
        {
            invalidateGpuBufferVmaAllocation(
                pick_device_->vmaAllocator(),
                static_cast<VmaAllocation>(
                    frame.candidate_dispatch_allocation),
                0u,
                sizeof(CandidateDispatchState));
            const auto* dispatch = static_cast<const CandidateDispatchState*>(
                frame.candidate_dispatch_mapped);
            latest_gpu_candidate_requested_count_ = dispatch->requested;
            latest_gpu_candidate_count_ = dispatch->accepted;
            latest_gpu_candidate_overflow_count_ = dispatch->overflow;
            latest_gpu_candidate_group_count_ =
                dispatch->dispatch_group_count_x;
            has_gpu_candidate_count_ = true;
            const auto expected_groups =
                (latest_gpu_candidate_count_ + 63u) / 64u;
            gpu_candidate_dispatch_valid_ =
                dispatch->requested ==
                    dispatch->accepted + dispatch->overflow &&
                dispatch->dispatch_group_count_x == expected_groups &&
                dispatch->dispatch_group_count_y ==
                    (expected_groups == 0u ? 0u : 1u) &&
                dispatch->dispatch_group_count_z ==
                    (expected_groups == 0u ? 0u : 1u);
            frame.candidate_submitted = false;
        }
        if (frame.uploaded_revision == gpu_cull_revision_)
            return true;

        if (!frame.cluster_mapped || !frame.instance_mapped)
            return false;
        frame.cluster_count = static_cast<std::uint32_t>(
            gpu_cull_clusters_.size());
        frame.instance_count = static_cast<std::uint32_t>(
            gpu_cull_instances_.size());
        if (!gpu_cull_clusters_.empty())
        {
            std::memcpy(
                frame.cluster_mapped,
                gpu_cull_clusters_.data(),
                gpu_cull_clusters_.size() * sizeof(GpuCullCluster));
            flushGpuBufferVmaAllocation(
                pick_device_->vmaAllocator(),
                static_cast<VmaAllocation>(frame.cluster_allocation),
                0u,
                static_cast<VkDeviceSize>(gpu_cull_clusters_.size()) *
                    sizeof(GpuCullCluster));
        }
        if (!gpu_cull_instances_.empty())
        {
            std::memcpy(
                frame.instance_mapped,
                gpu_cull_instances_.data(),
                gpu_cull_instances_.size() * sizeof(GpuCullInstance));
            flushGpuBufferVmaAllocation(
                pick_device_->vmaAllocator(),
                static_cast<VmaAllocation>(frame.instance_allocation),
                0u,
                static_cast<VkDeviceSize>(gpu_cull_instances_.size()) *
                    sizeof(GpuCullInstance));
        }
        frame.uploaded_revision = gpu_cull_revision_;
        return true;
    }

    std::uint32_t RenderClusterResources::gpuCullBufferCount() const noexcept
    {
        return static_cast<std::uint32_t>(gpu_cull_frames_.size());
    }

    VkBuffer RenderClusterResources::gpuCullClusterBuffer(
        std::uint32_t index) const noexcept
    {
        return index < gpu_cull_frames_.size()
            ? gpu_cull_frames_[index].cluster_buffer
            : VK_NULL_HANDLE;
    }

    VkBuffer RenderClusterResources::gpuCullInstanceBuffer(
        std::uint32_t index) const noexcept
    {
        return index < gpu_cull_frames_.size()
            ? gpu_cull_frames_[index].instance_buffer
            : VK_NULL_HANDLE;
    }

    VkBuffer RenderClusterResources::gpuCandidateDispatchBuffer(
        std::uint32_t index) const noexcept
    {
        return index < gpu_cull_frames_.size()
            ? gpu_cull_frames_[index].candidate_dispatch_buffer
            : VK_NULL_HANDLE;
    }

    void RenderClusterResources::markGpuCandidateSubmitted(
        std::uint32_t frame_index) noexcept
    {
        if (gpu_cull_frames_.empty())
            return;
        gpu_cull_frames_[frame_index % gpu_cull_frames_.size()].
            candidate_submitted = true;
    }

    std::uint32_t RenderClusterResources::gpuCullClusterCount(
        std::uint32_t frame_index) const noexcept
    {
        return gpu_cull_frames_.empty()
            ? 0u
            : gpu_cull_frames_[frame_index % gpu_cull_frames_.size()].
                cluster_count;
    }

    std::uint32_t RenderClusterResources::gpuCullInstanceCount(
        std::uint32_t frame_index) const noexcept
    {
        return gpu_cull_frames_.empty()
            ? 0u
            : gpu_cull_frames_[frame_index % gpu_cull_frames_.size()].
                instance_count;
    }

    void RenderClusterResources::onPickingFrameBegin(
        std::uint32_t frame_index) noexcept
    {
        ++pick_frame_serial_;
        auto retired = retired_pick_tokens_.begin();
        while (retired != retired_pick_tokens_.end())
        {
            if (retired->retire_serial > pick_frame_serial_)
            {
                ++retired;
                continue;
            }
            if (pick_ids_.erase(retired->token) == 1u)
                free_pick_tokens_.push_back(retired->token);
            retired = retired_pick_tokens_.erase(retired);
        }
        if (pick_device_ == nullptr || pick_gpu_slots_.empty())
            return;
        auto& slot = pick_gpu_slots_[frame_index % pick_gpu_slots_.size()];
        if (slot.submitted && slot.mapped != nullptr)
        {
            invalidateGpuBufferVmaAllocation(
                pick_device_->vmaAllocator(),
                static_cast<VmaAllocation>(slot.allocation),
                0u,
                sizeof(std::uint32_t));
            const auto packed = *static_cast<const std::uint32_t*>(
                slot.mapped);
            latest_pick_ = {};
            latest_pick_.request_generation =
                slot.request.request_generation;
            latest_pick_.view_generation = slot.request.view_generation;
            if (packed == std::numeric_limits<std::uint32_t>::max())
            {
                latest_pick_.status = ERenderPickStatus::MISS;
            }
            else
            {
                const auto token = packed & kMaximumPickToken;
                const auto stable = resolvePickToken(token);
                if (!stable)
                {
                    latest_pick_.status = ERenderPickStatus::STALE;
                }
                else
                {
                    constexpr auto depth_levels =
                        (1u << (32u - kPickTokenBits)) - 1u;
                    const auto quantized_depth = packed >> kPickTokenBits;
                    const auto normalized = static_cast<double>(
                        quantized_depth) / depth_levels;
                    latest_pick_.stable_pick_id = *stable;
                    latest_pick_.depth = static_cast<float>(
                        std::exp2(normalized * std::log2(
                            1.0 + slot.request.maximum_distance)) - 1.0);
                    latest_pick_.status = ERenderPickStatus::HIT;
                }
            }
        }
        slot.submitted = false;
        slot.request = {};
        if (slot.mapped != nullptr)
        {
            *static_cast<std::uint32_t*>(slot.mapped) =
                std::numeric_limits<std::uint32_t>::max();
            flushGpuBufferVmaAllocation(
                pick_device_->vmaAllocator(),
                static_cast<VmaAllocation>(slot.allocation),
                0u,
                sizeof(std::uint32_t));
        }
    }

    void RenderClusterResources::requestPick(
        const RequestRenderClusterPickPayload& request) noexcept
    {
        if (request.request_generation == 0u ||
            !std::isfinite(request.normalized_x) ||
            !std::isfinite(request.normalized_y) ||
            request.normalized_x < 0.0f || request.normalized_x > 1.0f ||
            request.normalized_y < 0.0f || request.normalized_y > 1.0f ||
            !std::isfinite(request.maximum_distance) ||
            request.maximum_distance <= 0.0f)
        {
            latest_pick_ = {
                request.request_generation,
                0u,
                request.view_generation,
                ERenderPickStatus::FAILED,
                0.0f,
                0u};
            return;
        }
        pending_pick_ = request;
    }

    std::optional<RequestRenderClusterPickPayload>
    RenderClusterResources::pickRequestForView(
        std::uint32_t view_index) const noexcept
    {
        if (!pending_pick_ || pending_pick_->view_index != view_index)
            return std::nullopt;
        return pending_pick_;
    }

    void RenderClusterResources::markPickSubmitted(
        std::uint32_t frame_index,
        const RequestRenderClusterPickPayload& request) noexcept
    {
        if (pick_gpu_slots_.empty())
            return;
        auto& slot = pick_gpu_slots_[frame_index % pick_gpu_slots_.size()];
        slot.request = request;
        slot.submitted = true;
        if (pending_pick_ && pending_pick_->request_generation ==
                request.request_generation)
        {
            pending_pick_.reset();
        }
    }

    void RenderClusterResources::failPick(
        const RequestRenderClusterPickPayload& request,
        ERenderPickStatus status) noexcept
    {
        if (status != ERenderPickStatus::STALE)
            status = ERenderPickStatus::FAILED;
        latest_pick_ = {
            request.request_generation,
            0u,
            request.view_generation,
            status,
            0.0f,
            0u};
        if (pending_pick_ && pending_pick_->request_generation ==
                request.request_generation)
        {
            pending_pick_.reset();
        }
    }

    RenderClusterPickReply RenderClusterResources::pickResult(
        std::uint64_t request_generation) const noexcept
    {
        if (pending_pick_ && pending_pick_->request_generation ==
                request_generation)
        {
            return RenderClusterPickReply{
                request_generation,
                0u,
                pending_pick_->view_generation,
                ERenderPickStatus::PENDING,
                0.0f,
                0u};
        }
        for (const auto& slot : pick_gpu_slots_)
        {
            if (slot.submitted && slot.request.request_generation ==
                    request_generation)
            {
                return RenderClusterPickReply{
                    request_generation,
                    0u,
                    slot.request.view_generation,
                    ERenderPickStatus::PENDING,
                    0.0f,
                    0u};
            }
        }
        if (latest_pick_.request_generation == request_generation)
            return latest_pick_;
        RenderClusterPickReply stale{};
        stale.request_generation = request_generation;
        stale.status = ERenderPickStatus::STALE;
        return stale;
    }

    std::uint32_t RenderClusterResources::pickBufferCount() const noexcept
    {
        return static_cast<std::uint32_t>(pick_gpu_slots_.size());
    }

    VkBuffer RenderClusterResources::pickBuffer(
        std::uint32_t index) const noexcept
    {
        return index < pick_gpu_slots_.size()
            ? pick_gpu_slots_[index].buffer
            : VK_NULL_HANDLE;
    }
} // namespace lux::render
