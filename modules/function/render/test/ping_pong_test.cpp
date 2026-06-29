// ============================================================================
//  ping_pong_test.cpp  —  RG演进 M1 ping-pong resource basics (CPU-only)
//
//  Pins the two pieces of logic that the ping-pong resource model rests on,
//  without needing a Vulkan device:
//    1. RGBuilder::createPingPong lays out a CURRENT + PREVIOUS resource pair
//       (ring_phase 0/1, mutual pingpong_peer, shared ring_size).
//    2. resolveRingTarget redirects a PREVIOUS handle to the peer's earlier-
//       frame copy: for ring_size==FIF==2, prev@slot reads copy (slot+1)%2,
//       i.e. last frame's CURRENT write. CURRENT resolves as identity.
//
//  GPU end-to-end copy rotation is validated later by the HZB migration (the
//  first real ping-pong client). Self-checking: 0 = PASS, 1 = FAIL.
// ============================================================================

#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/graph/PhysicalResourceAllocator.hpp>

#include <cstdio>
#include <cstdint>

using namespace lux::render;

namespace
{
    int g_fails = 0;

    void check(bool ok, const char* what)
    {
        if (!ok) { std::printf("  FAIL: %s\n", what); ++g_fails; }
    }
}

int main()
{
    // ---- 1. createPingPong handle + descriptor layout ----
    {
        RGBuilder b;
        const auto desc = RGTextureDescription::Absolute(
            64u, 64u, lux::common::ETextureFormat::R32_SFLOAT);
        const RGRingResourceHandle pp = b.createPingPong("PP", desc, /*ring_size=*/2u);

        const auto& res = b.graphInternal().resources;
        check(res.size() == 2u, "createPingPong pushes exactly cur + prev");
        check(pp.current().index == 0u && pp.previous().index == 1u, "handle indices are cur=0, prev=1");

        if (res.size() == 2u)
        {
            const auto& cur  = res[0];
            const auto& prev = res[1];
            check(cur.lifetime == ERGResourceLifetime::PING_PONG, "cur lifetime PING_PONG");
            check(cur.ring_phase == 0u,  "cur ring_phase 0");
            check(cur.ring_size == 2u,   "cur ring_size 2");
            check(cur.pingpong_peer == 1, "cur peer -> prev");
            check(prev.lifetime == ERGResourceLifetime::PING_PONG, "prev lifetime PING_PONG");
            check(prev.ring_phase == 1u,  "prev ring_phase 1");
            check(prev.pingpong_peer == 0, "prev peer -> cur");
        }
    }

    // ---- 2. resolveRingTarget redirect math (ring_size == FIF == 2) ----
    {
        RGPhysicalResourceTable table;

        const auto i0 = static_cast<uint32_t>(table.emplace());
        {
            auto& cur = table.at(i0);
            cur.index            = i0;
            cur.lifetime         = ERGResourceLifetime::PING_PONG;
            cur.ring_phase       = 0u;
            cur.ring_size        = 2u;
            cur.pingpong_peer    = static_cast<int32_t>(i0 + 1u);
            cur.physical_handles = { 0xA1u, 0xB2u };   // copy 0, copy 1
        }
        const auto i1 = static_cast<uint32_t>(table.emplace());
        {
            auto& prev = table.at(i1);
            prev.index         = i1;
            prev.lifetime      = ERGResourceLifetime::PING_PONG;
            prev.ring_phase    = 1u;
            prev.ring_size     = 2u;
            prev.pingpong_peer = static_cast<int32_t>(i0);
            // no physical_handles — resolves through the peer
        }

        // CURRENT: identity (writes this frame's copy = slot).
        auto c0 = resolveRingTarget(table, i0, 0u);
        auto c1 = resolveRingTarget(table, i0, 1u);
        check(c0.first == i0 && c0.second == 0u, "cur@slot0 -> (cur, copy0)");
        check(c1.first == i0 && c1.second == 1u, "cur@slot1 -> (cur, copy1)");

        // PREVIOUS: redirect to peer, frame (slot + copies - phase) % copies = (slot+1)%2.
        // slot 1's prev reads copy 0 == slot 0's CURRENT write (last frame). slot 0's
        // prev reads copy 1 (not yet written on the very first frame — guard territory).
        auto p0 = resolveRingTarget(table, i1, 0u);
        auto p1 = resolveRingTarget(table, i1, 1u);
        check(p0.first == i0 && p0.second == 1u, "prev@slot0 -> (cur, copy1)");
        check(p1.first == i0 && p1.second == 0u, "prev@slot1 -> (cur, copy0) == last frame's write");
    }

    if (g_fails == 0)
        std::printf("ping_pong_test: PASS\n");
    else
        std::printf("ping_pong_test: FAIL (%d)\n", g_fails);
    return g_fails == 0 ? 0 : 1;
}
