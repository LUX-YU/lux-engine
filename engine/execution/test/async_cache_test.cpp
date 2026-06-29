// ============================================================================
//  async_cache_test — Phase C2. Self-checking exe (exit 0 on success, no GTest).
//
//  Validates the two execution-layer primitives Phase C's editor rewrites build on:
//    - AsyncCache<K,V> : main-thread cache (tryGet/state + driver markPending/setReady/
//      setFailed + fire-and-forget ensure + invalidate/clear). ensure runs over a real
//      EngineExecutor (spawn + mainScheduler + drainMain).
//    - whenAllReady(vector<RenderRequest<T>>) : dynamic-arity counting join — empty set,
//      N-pending, a synchronously-ready child, and a mixed set.
//  Deterministic + headless (no GPU / render server; the CPU pool runs `just` producers).
// ============================================================================

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

#include <stdexec/execution.hpp>

#include <lux/engine/execution/EngineExecutor.hpp>
#include <lux/engine/execution/AsyncCache.hpp>
#include <lux/engine/execution/RenderRequestJoin.hpp>

#include <lux/engine/asset/AssetManager.hpp>
#include <lux/engine/render/comm/client/RenderRequest.hpp>
#include <lux/engine/render/comm/FrameProgram.hpp>     // FrameReplies
#include <lux/engine/render/comm/RenderCommTypes.hpp>  // ReplyRecord

namespace ex = stdexec;

namespace
{
    int g_pass = 0, g_fail = 0;
    void check(bool cond, const char* desc)
    {
        if (cond) { std::printf("  PASS  %s\n", desc); ++g_pass; }
        else      { std::printf("  FAIL  %s\n", desc); ++g_fail; }
    }

    struct TestReply { std::uint32_t id{0}; bool ok{false}; };
    static_assert(std::is_trivially_copyable_v<TestReply>);
    using Factory = lux::render::RenderRequestFactory<TestReply>;

    // Fire a request's continuation exactly as RenderSession::pumpReplies() would.
    void completeViaCallback(const Factory::Callback& cb, TestReply value)
    {
        lux::render::FrameReplies<64> pkt;
        pkt.payload.resize(sizeof(TestReply));
        std::memcpy(pkt.payload.data(), &value, sizeof(TestReply));
        lux::render::ReplyRecord rec{};
        rec.payload_offset = 0;
        rec.payload_size   = sizeof(TestReply);
        cb(pkt, rec);
    }

    // Receiver recording a void set_value() (whenAllReady completes set_value()).
    struct VoidRecv
    {
        using receiver_concept = ex::receiver_t;
        bool* fired;
        void set_value() noexcept { *fired = true; }
        void set_error(std::exception_ptr) noexcept {}
        void set_stopped() noexcept {}
        [[nodiscard]] ex::empty_env get_env() const noexcept { return {}; }
    };

    // Pump drainMain until pred() or a bounded spin guard trips.
    template <class Pred>
    void pumpUntil(lux::exec::EngineExecutor& exec, Pred pred)
    {
        for (int i = 0; i < 1'000'000 && !pred(); ++i)
        {
            exec.drainMain();
            std::this_thread::yield();
        }
    }
}

int main()
{
    std::printf("AsyncCache + whenAllReady acceptance test\n");
    std::printf("=========================================\n");

    lux::asset::AssetManager  mgr;
    lux::exec::EngineExecutor exec(mgr, /*cpu_threads=*/2);

    // ── A: AsyncCache pure map ops (driver API) ─────────────────────────────
    {
        lux::exec::AsyncCache<std::uint32_t, std::uint32_t> cache(exec);
        check(cache.state(1) == lux::exec::CacheState::Pending, "A: missing key reads Pending");
        check(cache.tryGet(1) == nullptr,                        "A: missing key tryGet is null");
        check(cache.markPending(1),                              "A: markPending inserts (true)");
        check(!cache.markPending(1),                             "A: markPending dedups (false)");
        cache.setReady(1, 42u);
        check(cache.state(1) == lux::exec::CacheState::Ready,    "A: setReady -> Ready");
        check(cache.tryGet(1) && *cache.tryGet(1) == 42u,        "A: tryGet returns stored value");
        cache.markPending(2);
        cache.setFailed(2);
        check(cache.state(2) == lux::exec::CacheState::Failed,   "A: setFailed -> Failed");
        check(cache.tryGet(2) == nullptr,                        "A: Failed tryGet is null");
        cache.invalidate(1);
        check(cache.state(1) == lux::exec::CacheState::Pending,  "A: invalidate erases (back to missing)");
        cache.clear();
        check(cache.state(2) == lux::exec::CacheState::Pending,  "A: clear erases all");
    }

    // ── B: ensure -> Ready + dedup ──────────────────────────────────────────
    {
        lux::exec::AsyncCache<std::uint32_t, std::uint32_t> cache(exec);
        cache.ensure(5, [] { return ex::just(std::uint32_t{99}); });
        check(cache.state(5) == lux::exec::CacheState::Pending, "B: ensure starts Pending (store deferred to drainMain)");
        pumpUntil(exec, [&] { return cache.state(5) == lux::exec::CacheState::Ready; });
        check(cache.tryGet(5) && *cache.tryGet(5) == 99u,       "B: ensure stored the produced value after drainMain");

        // second ensure on a present key is a no-op (no re-spawn): a different producer must NOT overwrite.
        cache.ensure(5, [] { return ex::just(std::uint32_t{7}); });
        pumpUntil(exec, [&] { return exec.drainMain() == 0; });
        check(cache.tryGet(5) && *cache.tryGet(5) == 99u,       "B: ensure dedups (present key not re-produced)");
    }

    // ── C: ensure failure path (producer set_error -> Failed) ───────────────
    {
        lux::exec::AsyncCache<std::uint32_t, std::uint32_t> cache(exec);
        cache.ensure(6, []
        {
            return ex::just(std::uint32_t{0})
                 | ex::then([](std::uint32_t) -> std::uint32_t { throw std::runtime_error("boom"); });
        });
        pumpUntil(exec, [&] { return cache.state(6) == lux::exec::CacheState::Failed; });
        check(cache.state(6) == lux::exec::CacheState::Failed,  "C: producer error -> Failed");
        check(cache.tryGet(6) == nullptr,                       "C: Failed tryGet is null");
    }

    // ── D: whenAllReady ─────────────────────────────────────────────────────
    {
        bool fired = false;
        auto op = ex::connect(lux::exec::whenAllReady<TestReply>({}), VoidRecv{&fired});
        ex::start(op);
        check(fired, "D: whenAllReady(empty) completes synchronously");
    }
    {
        bool fired = false;
        auto r1 = Factory::make();
        auto r2 = Factory::make();
        std::vector<lux::render::RenderRequest<TestReply>> v{ r1.request, r2.request };
        auto op = ex::connect(lux::exec::whenAllReady<TestReply>(std::move(v)), VoidRecv{&fired});
        ex::start(op);
        check(!fired, "D: whenAllReady(2 pending) does not fire early");
        completeViaCallback(r1.callback, TestReply{1, true});
        check(!fired, "D: still pending after 1/2");
        completeViaCallback(r2.callback, TestReply{2, true});
        check(fired, "D: fires after all ready");
    }
    {
        bool fired = false;
        std::vector<lux::render::RenderRequest<TestReply>> v{ Factory::makeImmediate(TestReply{5, true}) };
        auto op = ex::connect(lux::exec::whenAllReady<TestReply>(std::move(v)), VoidRecv{&fired});
        ex::start(op);
        check(fired, "D: whenAllReady(single already-ready) completes synchronously");
    }
    {
        bool fired = false;
        auto rp = Factory::make();
        std::vector<lux::render::RenderRequest<TestReply>> v{ Factory::makeImmediate(TestReply{1, true}), rp.request };
        auto op = ex::connect(lux::exec::whenAllReady<TestReply>(std::move(v)), VoidRecv{&fired});
        ex::start(op);
        check(!fired, "D: mixed (immediate+pending) waits on the pending one");
        completeViaCallback(rp.callback, TestReply{2, true});
        check(fired, "D: mixed fires once the pending one settles");
    }

    exec.shutdown();

    std::printf("\n=========================================\n");
    std::printf("  Summary: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
