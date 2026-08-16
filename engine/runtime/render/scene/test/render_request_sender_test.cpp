//=============================================================================
// render_request_sender_test.cpp
//-----------------------------------------------------------------------------
// Phase B: deterministic, headless (no GPU / no render server / no threads)
// acceptance test for the runtime render integration's request sender adapter.
// adapter over the render reply primitive.
//
// The COMPILE itself is the primary stdexec-conformance check: if the sender /
// op-state are non-conforming, stdexec::connect / when_all / sync_wait fail to
// instantiate below. The runtime checks exercise value, typed-error, composition,
// move-only continuation and RAII-detach paths:
//   1. already-ready request  -> set_value fires SYNCHRONOUSLY inside start();
//      + sync_wait round-trips the value.
//   2. pending request        -> parks; completes when the factory callback runs
//      (faithfully reproducing the real RenderFrameSession::pumpReplies path).
//   3. composition            -> when_all(asSender, asSender) | then | sync_wait.
//=============================================================================

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>

#include <stdexec/execution.hpp>

#include <lux/engine/runtime/extensions/detail/RenderRequestSender.hpp>
#include <lux/engine/function/render/client/RenderClient.hpp>
#include <lux/engine/function/render/client/RenderRequest.hpp>
#include <lux/engine/function/render/client/FrameProgram.hpp>     // ReplyPacket
#include <lux/engine/function/render/client/protocol/RenderCommTypes.hpp>  // ReplyRecord

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
        bool* errored{nullptr};
        lux::render::RenderError* error{nullptr};
        void set_value(T v) noexcept { *out = v; *fired = true; }
        void set_error(lux::render::RenderError e) noexcept
        {
            if (error != nullptr) *error = e;
            if (errored != nullptr) *errored = true;
        }
        void set_stopped() noexcept { std::printf("  (unexpected set_stopped)\n"); }
        [[nodiscard]] stdexec::env<> get_env() const noexcept { return {}; }
    };

    using Factory = lux::render::RenderRequestFactory<TestReply>;

    // Build a minimal reply packet + record and run the factory callback — exactly
    // what RenderFrameSession::pumpReplies() does when the server's reply lands.
    void completeViaCallback(const Factory::Callback& cb, TestReply value)
    {
        lux::render::ReplyPacket<64> pkt;          // non-copyable; built in place
        pkt.payload.resize(sizeof(TestReply));
        std::memcpy(pkt.payload.data(), &value, sizeof(TestReply));

        lux::render::ReplyRecord rec{};
        rec.payload_offset = 0;
        rec.payload_size   = sizeof(TestReply);
        cb(pkt, rec);
    }

    void completeViaFailure(const Factory::Callback& cb,
                            lux::render::RenderError  error)
    {
        const lux::render::CommandFailedReply failure{17u, error};
        lux::render::ReplyPacket<64> pkt;
        pkt.payload.resize(sizeof(failure));
        std::memcpy(pkt.payload.data(), &failure, sizeof(failure));

        lux::render::ReplyRecord rec{};
        rec.type_id        = lux::render::kReplyCommandFailedTypeId;
        rec.payload_offset = 0;
        rec.payload_size   = sizeof(failure);
        cb(pkt, rec);
    }

    void completeWithMalformedPayload(
        const Factory::Callback& cb,
        std::uint32_t offset,
        std::uint32_t size
    )
    {
        lux::render::ReplyPacket<64> packet;
        packet.payload.resize(sizeof(TestReply) - 1u);
        lux::render::ReplyRecord reply{};
        reply.payload_offset = offset;
        reply.payload_size = size;
        cb(packet, reply);
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
        auto op  = stdexec::connect(lux::runtime::render_detail::asSender(req),
                                    RecordRecv<TestReply>{&fired, &out});
        stdexec::start(op);
        check(fired, "case1: already-ready request completes synchronously in start()");
        check(out.id == 42 && out.ok, "case1: value carried through set_value");
    }

    // ── Case 1b: sync_wait round-trips an already-ready value ──
    {
        auto res = stdexec::sync_wait(
            lux::runtime::render_detail::asSender(Factory::makeImmediate(TestReply{7, false})));
        check(res.has_value(), "case1b: sync_wait completes on an immediate request");
        check(res && std::get<0>(*res).id == 7, "case1b: sync_wait yields the value");
    }

    // ── Case 2: pending -> parks, then completes via the factory callback ──
    {
        bool      fired = false;
        TestReply out{};
        auto r  = Factory::make();                       // {request (pending), callback}
        auto op = stdexec::connect(lux::runtime::render_detail::asSender(r.request),
                                   RecordRecv<TestReply>{&fired, &out});
        stdexec::start(op);
        check(!fired, "case2: pending request does NOT fire before the reply");

        completeViaCallback(r.callback, TestReply{99, true});
        check(fired, "case2: set_value fires when the reply callback runs (pumpReplies path)");
        check(out.id == 99 && out.ok, "case2: value carried through");
    }

    // ── Case 2b: generic dispatch failure -> typed set_error, never value ──
    {
        bool fired   = false;
        bool errored = false;
        TestReply out{};
        lux::render::RenderError observed{};
        auto r = Factory::make();
        auto op = stdexec::connect(
            lux::runtime::render_detail::asSender(r.request),
            RecordRecv<TestReply>{&fired, &out, &errored, &observed});
        stdexec::start(op);

        const auto expected = lux::render::makeError(
            lux::render::ErrorTypeId{12u, 3u}, 41u, 42u, 43u);
        completeViaFailure(r.callback, expected);
        check(errored, "case2b: dispatch failure completes through set_error");
        check(!fired, "case2b: dispatch failure never masquerades as a value");
        check(observed.type == expected.type && observed.args == expected.args,
              "case2b: RenderError payload is preserved");
    }

    // ── Case 2c: move-only continuation + lexical RAII detach ──
    {
        bool fired = false;
        auto r = Factory::make();
        {
            lux::render::ScopedRenderRequest scoped(std::move(r.request));
            scoped.then([guard = std::make_unique<int>(9), &fired]
                        (const TestReply&) mutable
                        { fired = (*guard == 9); });
        }
        completeViaCallback(r.callback, TestReply{123, true});
        check(!fired, "case2c: ScopedRenderRequest destructor detaches continuation");
    }

    {
        lux::render::RenderRequest<TestReply> invalid;
        auto invalid_result = invalid.tryResult();
        check(
            !invalid_result &&
                invalid_result.error().type ==
                    lux::render::renderError<
                        lux::render::err::comm::RequestInvalid>().type,
            "case2e: stateless request reports RequestInvalid"
        );

        auto pending = Factory::make();
        auto pending_result = pending.request.tryResult();
        check(
            !pending_result &&
                pending_result.error().type ==
                    lux::render::renderError<
                        lux::render::err::comm::RequestNotReady>().type,
            "case2e: pending request reports RequestNotReady"
        );
    }

    {
        auto malformed_size = Factory::make();
        completeWithMalformedPayload(
            malformed_size.callback,
            0,
            sizeof(TestReply) - 1u
        );
        check(
            malformed_size.request.failed() &&
                malformed_size.request.error().type ==
                    lux::render::renderError<
                        lux::render::err::comm::PayloadSizeMismatch>().type,
            "case2f: wrong-sized reply settles as PayloadSizeMismatch"
        );

        auto out_of_bounds = Factory::make();
        completeWithMalformedPayload(
            out_of_bounds.callback,
            sizeof(TestReply),
            sizeof(TestReply)
        );
        check(
            out_of_bounds.request.failed() &&
                out_of_bounds.request.error().type ==
                    lux::render::renderError<
                        lux::render::err::comm::PayloadOutOfBounds>().type,
            "case2f: out-of-bounds reply settles as PayloadOutOfBounds"
        );
    }

    {
        constexpr lux::render::TypeId expected_type = 71;
        constexpr lux::render::TypeId wrong_type = 72;
        lux::render::ResponseCallbackStore<> callbacks;
        auto result = Factory::make();
        const auto request_id = callbacks.registerCallback(
            std::move(result.callback),
            expected_type
        );
        lux::render::ReplyPacket<> packet;
        lux::render::ReplyRecord reply{};
        reply.request_id = request_id;
        reply.type_id = wrong_type;
        packet.replies.push_back(reply);

        check(
            callbacks.dispatchAll(packet) == 1 &&
                result.request.failed() &&
                result.request.error().type ==
                    lux::render::renderError<
                        lux::render::err::comm::ReplyTypeMismatch>().type &&
                callbacks.malformedReplies() == 1,
            "case2g: reply type mismatch settles exactly once"
        );
        check(
            callbacks.dispatchAll(packet) == 0 &&
                callbacks.malformedReplies() == 1,
            "case2g: duplicate mismatched reply cannot settle twice"
        );
    }

    // ── Case 2d: completion may erase its own lexical owner ──
    // ECS pending maps use this exact shape. The factory must move the callback
    // out of shared State before invocation, otherwise erasing the token destroys
    // the currently executing callable.
    {
        bool fired = false;
        auto r = Factory::make();
        std::optional<lux::render::ScopedRenderRequest<TestReply>> scoped;
        scoped.emplace(std::move(r.request));
        scoped->then([&](const TestReply& reply)
        {
            scoped.reset();
            fired = (reply.id == 321u);
        });
        completeViaCallback(r.callback, TestReply{321, true});
        check(fired && !scoped,
              "case2d: callback safely erases its own ScopedRenderRequest");
    }

    // ── Case 3: composition — when_all(asSender, asSender) | then | sync_wait ──
    {
        auto pipe =
            stdexec::when_all(
                lux::runtime::render_detail::asSender(Factory::makeImmediate(TestReply{3, true})),
                lux::runtime::render_detail::asSender(Factory::makeImmediate(TestReply{4, true})))
            | stdexec::then([](TestReply a, TestReply b) { return a.id + b.id; });
        auto res = stdexec::sync_wait(std::move(pipe));
        check(res.has_value(), "case3: when_all | then completes");
        check(res && std::get<0>(*res) == 7u, "case3: combined value (3 + 4)");
    }

    std::printf("\n=======================================================\n");
    std::printf("  Summary: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
