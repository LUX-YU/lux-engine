#include <lux/engine/render/renderer/features/render_cluster/RenderClusterResources.hpp>

#include <cassert>
#include <limits>
#include <vector>

int main()
{
    using namespace lux::render;

    RenderClusterResources resources;
    UploadRenderClusterPayload upload{};
    upload.scene_id = RenderSceneId{0u, 1u};
    upload.id.bytes[0] = 0x42u;
    upload.revision = 1u;
    upload.bounds_radius = 32.0f;
    upload.bounds_center.page_delta[0] = 20;
    upload.instance_count = 2u;
    std::vector<RenderClusterWireInstance> instances(2u);
    instances[0].stable_pick_id = 100u;
    instances[0].transform.page_delta[0] = 11;
    instances[1].stable_pick_id = 101u;
    instances[1].transform.page_delta[0] = 12;
    assert(resources.upsert(upload, instances));
    assert(resources.clusterCount() == 1u &&
        resources.instanceCount() == 2u);
    assert(resources.find(upload.id) &&
        resources.find(upload.id)->instances[1].stable_pick_id == 101u);
    const auto initial_memory = resources.cpuMemorySnapshot();
    assert(initial_memory.capacity_bytes != 0u);
    assert(initial_memory.allocation_count != 0u);

    const std::int64_t origin_delta[3]{10, 0, 0};
    assert(resources.canRebaseSceneOrigin(origin_delta));
    resources.rebaseSceneOrigin(origin_delta);
    assert(resources.find(upload.id)->header.bounds_center.page_delta[0] ==
        10);
    assert(resources.find(upload.id)->instances[0].transform.page_delta[0] ==
        1);
    assert(resources.find(upload.id)->instances[1].transform.page_delta[0] ==
        2);
    const std::int64_t rejected_delta[3]{
        std::numeric_limits<std::int64_t>::max(), 0, 0};
    assert(!resources.canRebaseSceneOrigin(rejected_delta));
    assert(resources.find(upload.id)->header.bounds_center.page_delta[0] ==
        10);

    // Control and upload lanes can overtake one another. A newer tombstone
    // must keep an older in-flight upload from resurrecting the Cluster.
    assert(resources.remove(upload.id, 3u));
    assert(resources.clusterCount() == 0u &&
        resources.instanceCount() == 0u);
    upload.revision = 2u;
    assert(resources.upsert(upload, instances));
    assert(resources.clusterCount() == 0u);

    upload.revision = 4u;
    upload.instance_count = 1u;
    assert(resources.upsert(
        upload,
        std::span<const RenderClusterWireInstance>{instances.data(), 1u}));
    assert(resources.clusterCount() == 1u &&
        resources.instanceCount() == 1u);
    assert(resources.remove(upload.id, 5u));
    assert(resources.clusterCount() == 0u);

    // GPU-visible pick tokens are compact, but their logical identity remains
    // the full stable 64-bit World ID. A removed token stays resolvable until
    // the FIF retirement horizon passes, so a late GPU result cannot alias a
    // newly uploaded object.
    RenderClusterResources picking;
    const auto cancelled_token = picking.allocatePickToken(0x1'0000'0042ull);
    assert(cancelled_token != 0u);
    assert(picking.resolvePickToken(cancelled_token) ==
        0x1'0000'0042ull);
    picking.cancelPickToken(cancelled_token);
    assert(!picking.resolvePickToken(cancelled_token));

    const auto retired_token = picking.allocatePickToken(0x2'0000'0042ull);
    assert(retired_token == cancelled_token);
    UploadRenderClusterPayload pick_upload{};
    pick_upload.scene_id = RenderSceneId{0u, 1u};
    pick_upload.id.bytes[0] = 0x51u;
    pick_upload.revision = 1u;
    pick_upload.bounds_radius = 1.0f;
    pick_upload.instance_count = 1u;
    assert(picking.upsert(
        pick_upload,
        std::span<const RenderClusterWireInstance>{instances.data(), 1u},
        {},
        {retired_token}));
    assert(picking.remove(pick_upload.id, 2u));
    assert(picking.resolvePickToken(retired_token) ==
        0x2'0000'0042ull);
    picking.onPickingFrameBegin(0u);
    assert(picking.resolvePickToken(retired_token));
    picking.onPickingFrameBegin(0u);
    assert(!picking.resolvePickToken(retired_token));

    RequestRenderClusterPickPayload pending{};
    pending.scene_id = RenderSceneId{0u, 1u};
    pending.view_index = 3u;
    pending.view_generation = 7u;
    pending.request_generation = 11u;
    pending.maximum_distance = 100.0f;
    picking.requestPick(pending);
    const auto pending_result = picking.pickResult(11u);
    assert(pending_result.status == ERenderPickStatus::PENDING);
    assert(pending_result.view_generation == 7u);
    const auto stale_result = picking.pickResult(12u);
    assert(stale_result.status == ERenderPickStatus::STALE);

    auto invalid = pending;
    invalid.request_generation = 13u;
    invalid.normalized_x = -1.0f;
    picking.requestPick(invalid);
    assert(picking.pickResult(13u).status == ERenderPickStatus::FAILED);

    // HLOD readiness is a bounded family transition: the parent remains
    // visible while only a prefix of its children is resident. The last child
    // atomically hides the parent and exposes all children. Losing any child
    // reverses that order, so there is never an empty representation.
    RenderClusterResources hierarchy;
    UploadRenderClusterPayload parent{};
    parent.scene_id = RenderSceneId{0u, 1u};
    parent.id.bytes[0] = 0xa0u;
    parent.revision = 1u;
    parent.bounds_radius = 64.0f;
    parent.child_count = 2u;
    parent.children[0].bytes[0] = 0xa1u;
    parent.children[1].bytes[0] = 0xa2u;
    parent.instance_count = 1u;
    assert(hierarchy.upsert(
        parent,
        std::span<const RenderClusterWireInstance>{instances.data(), 1u}));
    (void)hierarchy.reconcileHierarchy(parent.id);
    assert(hierarchy.find(parent.id)->visible);
    assert(hierarchy.visibleClusterCount() == 1u &&
        hierarchy.visibleInstanceCount() == 1u);

    auto child = parent;
    child.parent = parent.id;
    child.child_count = 0u;
    child.id = parent.children[0];
    assert(hierarchy.upsert(
        child,
        std::span<const RenderClusterWireInstance>{instances.data(), 1u}));
    (void)hierarchy.reconcileHierarchy(parent.id);
    assert(hierarchy.find(parent.id)->visible);
    assert(!hierarchy.find(child.id)->visible);

    child.id = parent.children[1];
    assert(hierarchy.upsert(
        child,
        std::span<const RenderClusterWireInstance>{instances.data(), 1u}));
    (void)hierarchy.reconcileHierarchy(parent.id);
    assert(!hierarchy.find(parent.id)->visible);
    assert(hierarchy.find(parent.children[0])->visible);
    assert(hierarchy.find(parent.children[1])->visible);
    assert(hierarchy.visibleClusterCount() == 2u &&
        hierarchy.visibleInstanceCount() == 2u);
    (void)hierarchy.reconcileHierarchy(parent.id, false);
    assert(hierarchy.find(parent.id)->visible);
    assert(!hierarchy.find(parent.children[0])->visible);
    assert(!hierarchy.find(parent.children[1])->visible);
    (void)hierarchy.reconcileHierarchy(parent.id, true);
    assert(!hierarchy.find(parent.id)->visible);
    assert(hierarchy.find(parent.children[0])->visible);
    assert(hierarchy.find(parent.children[1])->visible);

    assert(hierarchy.remove(parent.children[1], 2u));
    (void)hierarchy.reconcileHierarchy(parent.id);
    assert(hierarchy.find(parent.id)->visible);
    assert(!hierarchy.find(parent.children[0])->visible);
    assert(hierarchy.visibleClusterCount() == 1u);

    assert(hierarchy.remove(parent.id, 2u));
    (void)hierarchy.reconcileHierarchy(parent.id);
    assert(hierarchy.find(parent.children[0])->visible);
    assert(hierarchy.visibleClusterCount() == 1u);

    // A multi-level tree is reconciled root-to-leaf as one visibility state.
    // Selecting the top parent hides every descendant; selecting its children
    // allows each child family to make its own LOD choice without exposing
    // both an ancestor and a grandchild.
    RenderClusterResources multilevel;
    auto root = parent;
    root.id.bytes[0] = 0xb0u;
    root.children[0].bytes[0] = 0xb1u;
    root.children[1].bytes[0] = 0xb2u;
    assert(multilevel.upsert(
        root,
        std::span<const RenderClusterWireInstance>{instances.data(), 1u}));

    auto middle = parent;
    middle.id = root.children[0];
    middle.parent = root.id;
    middle.children[0].bytes[0] = 0xb3u;
    middle.children[1].bytes[0] = 0xb4u;
    assert(multilevel.upsert(
        middle,
        std::span<const RenderClusterWireInstance>{instances.data(), 1u}));
    auto middle_leaf = child;
    middle_leaf.id = root.children[1];
    middle_leaf.parent = root.id;
    assert(multilevel.upsert(
        middle_leaf,
        std::span<const RenderClusterWireInstance>{instances.data(), 1u}));
    auto leaf = child;
    leaf.id = middle.children[0];
    leaf.parent = middle.id;
    assert(multilevel.upsert(
        leaf,
        std::span<const RenderClusterWireInstance>{instances.data(), 1u}));
    leaf.id = middle.children[1];
    assert(multilevel.upsert(
        leaf,
        std::span<const RenderClusterWireInstance>{instances.data(), 1u}));

    (void)multilevel.reconcileHierarchy(root.id, true);
    (void)multilevel.reconcileHierarchy(middle.id, true);
    assert(!multilevel.find(root.id)->visible);
    assert(!multilevel.find(middle.id)->visible);
    assert(multilevel.find(root.children[1])->visible);
    assert(multilevel.find(middle.children[0])->visible);
    assert(multilevel.find(middle.children[1])->visible);

    (void)multilevel.reconcileHierarchy(root.id, false);
    assert(multilevel.find(root.id)->visible);
    assert(!multilevel.find(middle.id)->visible);
    assert(!multilevel.find(root.children[1])->visible);
    assert(!multilevel.find(middle.children[0])->visible);

    (void)multilevel.reconcileHierarchy(root.id, true);
    (void)multilevel.reconcileHierarchy(middle.id, false);
    assert(!multilevel.find(root.id)->visible);
    assert(multilevel.find(middle.id)->visible);
    assert(multilevel.find(root.children[1])->visible);
    assert(!multilevel.find(middle.children[0])->visible);
    assert(!multilevel.find(middle.children[1])->visible);

    // Timed transitions keep both representations drawable until the GPU
    // coverage interval completes. A reverse request preserves the current
    // coverage instead of restarting at an endpoint.
    RenderClusterResources transitioning;
    assert(transitioning.upsert(
        parent,
        std::span<const RenderClusterWireInstance>{instances.data(), 1u}));
    child.id = parent.children[0];
    assert(transitioning.upsert(
        child,
        std::span<const RenderClusterWireInstance>{instances.data(), 1u}));
    child.id = parent.children[1];
    assert(transitioning.upsert(
        child,
        std::span<const RenderClusterWireInstance>{instances.data(), 1u}));
    (void)transitioning.reconcileHierarchy(
        parent.id, false, 1.0f, 0.35f);
    (void)transitioning.reconcileHierarchy(
        parent.id, false, 1.36f, 0.35f);
    assert(transitioning.find(parent.id)->visible);
    assert(!transitioning.find(parent.children[0])->visible);
    assert(transitioning.transitionCount() == 0u);

    const auto begin_changes = transitioning.reconcileHierarchy(
        parent.id, true, 2.0f, 0.35f);
    assert(!begin_changes.empty());
    assert(transitioning.find(parent.id)->visible);
    assert(transitioning.find(parent.children[0])->visible);
    assert(transitioning.find(parent.children[1])->visible);
    assert(transitioning.transitionCount() == 3u);
    (void)transitioning.reconcileHierarchy(
        parent.id, false, 2.1f, 0.35f);
    assert(transitioning.transitionCount() == 3u);
    (void)transitioning.reconcileHierarchy(
        parent.id, false, 2.46f, 0.35f);
    assert(transitioning.find(parent.id)->visible);
    assert(!transitioning.find(parent.children[0])->visible);
    assert(!transitioning.find(parent.children[1])->visible);
    assert(transitioning.transitionCount() == 0u);
    assert(RenderClusterResources::transitionSeed(
               42u, parent.id, 0u) ==
        RenderClusterResources::transitionSeed(
               42u, parent.id, 0u));
    return 0;
}
