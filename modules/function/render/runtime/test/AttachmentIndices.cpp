#include "Scene.hpp"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>

using namespace lux::render;
using namespace lux::render::runtime_detail;
namespace cxx = lux::cxx;

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #value); std::abort(); } } while (false)

static void exercise(std::size_t count)
{
    // Only the production relation/index algorithms are exercised here. No GPU objects or fake submissions.
    ResourceRegistry registry(nullptr, 0);
    cxx::StableSlotMap<RenderScene, SceneTag> scenes;
    cxx::SlotMap<RenderEffect, EffectTag> effects;
    cxx::StableSlotMap<RenderTarget, TargetTag> targets;
    cxx::SlotMap<EffectAttachment, AttachmentTag> attachments;
    const auto scene = scenes.emplace(registry);
    auto& views = scenes.find(scene)->views;
    std::array<ViewId, 7> view_ids;
    std::array<EffectId, 11> effect_ids;
    std::array<TargetId, 13> target_ids;
    for (auto& id : view_ids) id = views.emplace(registry, CameraState{});
    for (auto& id : effect_ids) id = effects.emplace();
    for (auto& id : target_ids) id = targets.emplace();
    std::vector<AttachmentId> ids;
    ids.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto view = view_ids[i % view_ids.size()];
        const auto effect = effect_ids[i % effect_ids.size()];
        const auto target = target_ids[i % target_ids.size()];
        const auto id = attachments.emplace(EffectAttachment{effect, scene, view, target, nullptr});
        attachments.find(id)->link(id, *views.find(view), *effects.find(effect), *targets.find(target));
        ids.push_back(id);
    }
    const auto validate = [&] {
        std::size_t total = 0;
        for (const auto& target : targets)
        {
            for (auto at = target.attachments.begin(); at != target.attachments.end(); ++at)
            {
                const auto* entry = attachments.find(*at);
                CHECK(entry && entry->target_position == at);
                CHECK(views.find(entry->view)->attachments[entry->view_position] == *at);
                CHECK(effects.find(entry->effect)->attachments[entry->effect_position] == *at);
                ++total;
            }
        }
        CHECK(total == attachments.size());
    };
    const auto erase = [&](AttachmentId id) {
        auto& entry = *attachments.find(id);
        entry.unlink(attachments, *views.find(entry.view), *effects.find(entry.effect), *targets.find(entry.target));
        CHECK(attachments.erase(id));
        CHECK(!attachments.find(id));
    };
    validate();
    // Growing the Target owner must preserve ordered-list node identities.
    for (unsigned i = 0; i < 100; ++i) targets.emplace();
    validate();
    // Remove interior entries. Swap removal must repair both reverse positions; Target order stays intact.
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < ids.size(); i += 2) erase(ids[i]);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    validate();
    for (const auto target_id : target_ids)
    {
        auto expected = targets.find(target_id)->attachments.begin();
        for (std::size_t i = 1; i < ids.size(); i += 2)
        {
            if (attachments.find(ids[i])->target != target_id) continue;
            CHECK(expected != targets.find(target_id)->attachments.end() && *expected++ == ids[i]);
        }
        CHECK(expected == targets.find(target_id)->attachments.end());
    }
    // Generation reuse never makes the removed ID valid, even when the local index is reused.
    const auto replacement = attachments.emplace(EffectAttachment{
        effect_ids[0], scene, view_ids[0], target_ids[0], nullptr
    });
    CHECK(replacement != ids[0] && !attachments.find(ids[0]));
    bool reused = false;
    for (const auto old : ids)
    {
        if (old.index != replacement.index) continue;
        CHECK(old.gen != replacement.gen && !attachments.find(old));
        reused = true;
        break;
    }
    CHECK(reused);
    attachments.find(replacement)->link(
        replacement, *views.find(view_ids[0]), *effects.find(effect_ids[0]), *targets.find(target_ids[0])
    );
    erase(replacement);
    // Delete one View through its own list, then one Effect through its own list.
    auto& view = *views.find(view_ids[0]);
    while (!view.attachments.empty()) erase(view.attachments.back());
    auto& effect = *effects.find(effect_ids[0]);
    while (!effect.attachments.empty()) erase(effect.attachments.back());
    validate();
    for (auto& target : targets)
        while (!target.attachments.empty()) erase(target.attachments.front());
    CHECK(attachments.empty());
    for (const auto& remaining : views) CHECK(remaining.attachments.empty());
    for (const auto& remaining : effects) CHECK(remaining.attachments.empty());
    std::printf("attachments=%zu unlink_count=%zu total_ns=%lld\n", count, (count + 1) / 2,
        static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
}

int main()
{
    for (const auto count : {1u, 100u, 10000u, 32768u}) exercise(count);
}
