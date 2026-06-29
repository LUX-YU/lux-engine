//=============================================================================
// render_request_sender_test.cpp
//-----------------------------------------------------------------------------
// Phase B: deterministic, headless (no GPU / no render server / no threads)
// acceptance test for lux::exec::asSender(RenderRequest<T>) — the stdexec sender
// adapter over the render reply primitive.
//
// The COMPILE itself is the primary stdexec-conformance check: if the sender /
// op-state are non-conforming, stdexec::connect / when_all / sync_wait fail to
// instantiate below. The runtime checks then exercise the three completion paths:
//   1. already-ready request  -> set_value fires SYNCHRONOUSLY inside start();
//      + sync_wait round-trips the value.
//   2. pending request        -> parks; completes when the factory callback runs
//      (faithfully reproducing the real RenderSession::pumpReplies path).
//   3. composition            -> when_all(asSender, asSender) | then | sync_wait.
//=============================================================================

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <tuple>
#include <utility>

#include <stdexec/execution.hpp>

#include <lux/engine/execution/RenderRequestSender.hpp>
#include <lux/engine/render/comm/client/RenderRequest.hpp>
#include <lux/engine/render/comm/FrameProgram.hpp>     // FrameReplies
#include <lux/engine/render/comm/RenderCommTypes.hpp>  // ReplyRecord

namespace
{
    int g_pass = 0;
    int g_fail = 0;
    void check(bool cond, const char* desc)
    {
        if (cond) { std::printf("  PASS  %s\n", desc); ++g_pass; }
        else      { std::printf("  FAIL  %s\n", desc); ++g_fail; }
    }

    // A trivially-copyable stand-in reply (RenderRequestFactory memcpy's sizeof(T)).
    struct TestReply
    {
        std::uint32_t id{0};
        bool          ok{false};
    };
    static_assert(std::is_trivially_copyable_v<TestReply>);

    // Minimal recording receiver (modern stdexec member-based receiver).
    template <class T>
    struct RecordRecv
    {
        using receiver_concept = stdexec::receiver_t;
        bool* fired;
        T*    out;
        void set_value(T v) noexcept { *out = v; *fired = true; }
        void set_error(std::exception_ptr) noexcept { std::printf("  (unexpected set_error)\n"); }
        void set_stopped() noexcept { std::printf("  (unexpected set_stopped)\n"); }
        [[nodiscard]] stdexec::empty_env get_env() const noexcept { return {}; }
    };

    using Factory = lux::render::RenderRequestFactory<TestReply>;

    // Build a minimal reply packet + record and run the factory callback — exactly
    // what RenderSession::pumpReplies() does when the server's reply lands.
    void completeViaCallback(const Factory::Callback& cb, TestReply value)
    {
        lux::render::FrameReplies<64> pkt;          // non-copyable; built in place
        pkt.payload.resize(sizeof(TestReply));
        std::memcpy(pkt.payload.data(), &value, sizeof(TestReply));

        lux::render::ReplyRecord rec{};
        rec.payload_offset = 0;
        rec.payload_size   = sizeof(TestReply);
        cb(pkt, rec);
    }
}

int main()
{
    std::printf("RenderRequest -> stdexec sender adapter acceptance test\n");
    std::printf("=======================================================\n");

    // ── Case 1: already-ready -> synchronous completion inside start() ──
    {
        bool      fired = false;
        TestReply out{};
        auto req = Factory::makeImmediate(TestReply{42, true});
        auto op  = stdexec::connect(lux::exec::asSender(req),
                                    RecordRecv<TestReply>{&fired, &out});
        stdexec::start(op);
        check(fired, "case1: already-ready request completes synchronously in start()");
        check(out.id == 42 && out.ok, "case1: value carried through set_value");
    }

    // ── Case 1b: sync_wait round-trips an already-ready value ──
    {
        auto res = stdexec::sync_wait(
            lux::exec::asSender(Factory::makeImmediate(TestReply{7, false})));
        check(res.has_value(), "case1b: sync_wait completes on an immediate request");
        check(res && std::get<0>(*res).id == 7, "case1b: sync_wait yields the value");
    }

    // ── Case 2: pending -> parks, then completes via the factory callback ──
    {
        bool      fired = false;
        TestReply out{};
        auto r  = Factory::make();                       // {request (pending), callback}
        auto op = stdexec::connect(lux::exec::asSender(r.request),
                                   RecordRecv<TestReply>{&fired, &out});
        stdexec::start(op);
        check(!fired, "case2: pending request does NOT fire before the reply");

        completeViaCallback(r.callback, TestReply{99, true});
        check(fired, "case2: set_value fires when the reply callback runs (pumpReplies path)");
        check(out.id == 99 && out.ok, "case2: value carried through");
    }

    // ── Case 3: composition — when_all(asSender, asSender) | then | sync_wait ──
    {
        auto pipe =
            stdexec::when_all(
                lux::exec::asSender(Factory::makeImmediate(TestReply{3, true})),
                lux::exec::asSender(Factory::makeImmediate(TestReply{4, true})))
            | stdexec::then([](TestReply a, TestReply b) { return a.id + b.id; });
        auto res = stdexec::sync_wait(std::move(pipe));
        check(res.has_value(), "case3: when_all | then completes");
        check(res && std::get<0>(*res) == 7u, "case3: combined value (3 + 4)");
    }

    std::printf("\n=======================================================\n");
    std::printf("  Summary: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
