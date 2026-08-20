#include <lux/engine/function/render/graph/DependencyAnalyzer.hpp>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    using namespace lux::render;

    struct OwnedPass
    {
        std::string name;
        phase_mask_t phase_mask{0};
        ERenderStage stage{ERenderStage::Default};
        std::vector<RGPassTextureRef> textures;
        std::vector<RGPassBufferRef> buffers;
        std::vector<std::string> after;
        std::vector<std::string> before;
    };

    std::vector<RGLogicalPassView> makeViews(
        const std::vector<OwnedPass>& passes
    )
    {
        std::vector<RGLogicalPassView> views;
        views.reserve(passes.size());
        for (const auto& pass : passes)
        {
            views.push_back(RGLogicalPassView{
                .name = pass.name,
                .phase_mask = pass.phase_mask,
                .stage = pass.stage,
                .textures = pass.textures,
                .buffers = pass.buffers,
                .after_passes = pass.after,
                .before_passes = pass.before
            });
        }
        return views;
    }

    bool check(bool condition, const char* message)
    {
        if (!condition)
            std::cerr << "render_graph dependency test: " << message << '\n';
        return condition;
    }
}

int main()
{
    using namespace lux::render;

    static_assert(sizeof(ETextureRole) == 4u);
    static_assert(static_cast<std::int32_t>(
        ETextureRole::COLOR_ATTACHMENT) == 0);
    static_assert(static_cast<std::int32_t>(
        ETextureRole::INPUT_ATTACHMENT) == 4);

    std::vector<OwnedPass> passes{
        OwnedPass{.name = "A"},
        OwnedPass{.name = "Z"},
        OwnedPass{.name = "B"}
    };
    passes[0].textures.push_back(RGPassTextureRef{
        .resource = RGResourceHandle{0},
        .role = lux::render::ETextureRole::COLOR_ATTACHMENT,
        .usage = ERGResourceUsage::WRITE
    });
    passes[1].textures.push_back(RGPassTextureRef{
        .resource = RGResourceHandle{0},
        .role = lux::render::ETextureRole::INPUT_ATTACHMENT,
        .usage = ERGResourceUsage::READ
    });

    auto views = makeViews(passes);
    const auto first = DependencyAnalyzer::analyze(RGLogicalGraphView{
        .resource_count = 1,
        .passes = views
    });
    const auto second = DependencyAnalyzer::analyze(RGLogicalGraphView{
        .resource_count = 1,
        .passes = views
    });

    bool ok = true;
    ok &= check(!first.has_cycle, "acyclic graph reported a cycle");
    ok &= check(
        first.pass_topological_order == std::vector<std::uint32_t>{0, 1, 2},
        "input-attachment consumer was not glued to its producer"
    );
    ok &= check(
        first.pass_topological_order == second.pass_topological_order,
        "topological order is not stable"
    );
    ok &= check(
        first.lifetime.size() == 1
            && first.lifetime[0].resource.index == 0
            && first.lifetime[0].first_pass == 0
            && first.lifetime[0].last_pass == 1,
        "resource lifetime is incorrect"
    );

    std::vector<OwnedPass> cyclic{
        OwnedPass{.name = "left", .after = {"right"}},
        OwnedPass{.name = "right", .after = {"left"}}
    };
    auto cyclic_views = makeViews(cyclic);
    const auto cycle = DependencyAnalyzer::analyze(RGLogicalGraphView{
        .resource_count = 0,
        .passes = cyclic_views
    });
    ok &= check(cycle.has_cycle, "explicit ordering cycle was not detected");

    // A consumer may be declared before a late contribution that produces its
    // forward-referenced resource.  The explicit producer edge must determine
    // both the execution order and the resource hazard direction; otherwise
    // the read is incorrectly considered to precede the write.
    std::vector<OwnedPass> forward_reference{
        OwnedPass{.name = "Tonemap", .stage = ERenderStage::PostProcess,
                  .after = {"WaterComposite"}},
        OwnedPass{.name = "WaterComposite", .stage = ERenderStage::PostProcess}
    };
    forward_reference[0].textures.push_back(RGPassTextureRef{
        .resource = RGResourceHandle{0},
        .role = lux::render::ETextureRole::SAMPLED,
        .usage = ERGResourceUsage::READ
    });
    forward_reference[1].textures.push_back(RGPassTextureRef{
        .resource = RGResourceHandle{0},
        .role = lux::render::ETextureRole::COLOR_ATTACHMENT,
        .usage = ERGResourceUsage::WRITE
    });
    auto forward_reference_views = makeViews(forward_reference);
    const auto ordered_forward_reference = DependencyAnalyzer::analyze(
        RGLogicalGraphView{
            .resource_count = 1,
            .passes = forward_reference_views
        }
    );
    ok &= check(
        !ordered_forward_reference.has_cycle &&
            ordered_forward_reference.pass_topological_order ==
                std::vector<std::uint32_t>{1, 0},
        "forward-referenced producer did not execute before its consumer"
    );

    return ok ? 0 : 1;
}
