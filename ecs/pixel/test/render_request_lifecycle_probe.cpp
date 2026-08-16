#include <lux/engine/ecs/render/TrackedRenderRequest.hpp>
#include <lux/engine/ecs/render/SceneRenderBinding.hpp>
#include <lux/engine/function/render/client/FeatureOpSend.hpp>
#include <lux/engine/function/render/client/FrameProgram.hpp>
#include <lux/engine/function/render/client/core/RenderErrorRegistry.hpp>
#include <lux/engine/function/render/client/protocol/RenderCommTypes.hpp>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

namespace lux::render
{
    struct MissingFeatureReply
    {
        std::uint32_t value{0};
    };

    struct MissingFeaturePayload
    {
        std::uint32_t value{0};
    };

    template <>
    struct CommandTraits<MissingFeaturePayload>
    {
        using Reply = MissingFeatureReply;

        static constexpr bool   has_reply     = true;
        static constexpr TypeId reply_type_id = reply_type_id_of_v<Reply>;
    };

    struct MissingFeatureOp
    {
        using Payload = MissingFeaturePayload;

        static constexpr EOperationLane lane = EOperationLane::Frame;
        static constexpr EOpKind    kind = EOpKind::Resource;
        static constexpr const char* name = "MissingFeature";
    };

    static_assert(FeatureOpDesc<MissingFeatureOp>);
}

namespace
{
    struct TestReply
    {
        std::uint32_t value{0};
    };
    static_assert(std::is_trivially_copyable_v<TestReply>);

    struct Intent
    {
        std::uint32_t value{0};
    };

    using Factory = lux::render::RenderRequestFactory<TestReply>;
    using Tracker = lux::ecs::TrackedRenderRequest<
        std::uint32_t,
        TestReply,
        Intent>;

    int failures = 0;

    void expect(bool condition, const char* message)
    {
        if (condition)
            return;
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }

    void complete(const Factory::Callback& callback, TestReply value)
    {
        lux::render::ReplyPacket<64> packet;
        packet.payload.resize(sizeof(value));
        std::memcpy(packet.payload.data(), &value, sizeof(value));

        lux::render::ReplyRecord record{};
        record.payload_offset = 0;
        record.payload_size = sizeof(value);
        callback(packet, record);
    }
}

int main()
{
    {
        Tracker tracker;
        const auto started = tracker.start(
            1u,
            Intent{7u},
            []
            {
                return Factory::makeImmediate(TestReply{11u});
            }
        );
        expect(started == lux::ecs::ETrackedRequestStart::STARTED,
               "immediate request starts");
        expect(tracker.contains(1u),
               "immediate request remains lexically owned before drain");
        expect(tracker.hasCompletions(),
               "immediate request enqueues a value completion");

        int completions = 0;
        tracker.drain(
            [&](Tracker::Completion completion)
            {
                ++completions;
                expect(completion.context.value == 7u,
                       "immediate completion preserves intent");
                expect(completion.reply.value == 11u,
                       "immediate completion preserves reply");
                expect(!completion.dispatch_failed,
                       "immediate value is not a dispatch failure");
            }
        );
        expect(completions == 1, "immediate completion drains exactly once");
        expect(!tracker.contains(1u), "drain retires immediate request");
        tracker.drain(
            [&](Tracker::Completion)
            {
                ++completions;
            }
        );
        expect(completions == 1,
               "a settled immediate request cannot complete twice");
    }

    {
        Tracker tracker;
        const auto error = lux::render::makeError(
            lux::render::ErrorTypeId{12u, 3u},
            41u,
            42u,
            43u
        );
        (void)tracker.start(
            2u,
            Intent{17u},
            [&error]
            {
                return Factory::makeImmediateFailure(error);
            }
        );

        int completions = 0;
        tracker.drain(
            [&](Tracker::Completion completion)
            {
                ++completions;
                expect(completion.dispatch_failed,
                       "immediate failure is structurally visible");
                expect(completion.context.value == 17u,
                       "immediate failure preserves cleanup intent");
                expect(completion.error.type == error.type,
                       "immediate failure preserves error type");
                expect(completion.error.args == error.args,
                       "immediate failure preserves error arguments");
            }
        );
        expect(completions == 1,
               "immediate failure settles exactly once");
        expect(!tracker.contains(2u),
               "immediate failure leaves no permanent pending entry");
    }

    {
        Tracker tracker;
        Factory::Callback old_callback;
        (void)tracker.start(
            3u,
            Intent{23u},
            [&old_callback]
            {
                auto issued = Factory::make();
                old_callback = std::move(issued.callback);
                return std::move(issued.request);
            }
        );
        expect(tracker.abandon(3u), "pending create can be abandoned");
        expect(!tracker.contains(3u),
               "abandon immediately frees the logical key");

        const auto restarted = tracker.start(
            3u,
            Intent{31u},
            []
            {
                return Factory::makeImmediate(TestReply{37u});
            }
        );
        expect(restarted == lux::ecs::ETrackedRequestStart::STARTED,
               "a re-entered owner starts before the old reply settles");

        int completions = 0;
        tracker.drain(
            [&](Tracker::Completion completion)
            {
                ++completions;
                expect(!completion.abandoned,
                       "replacement generation is current");
                expect(completion.context.value == 31u,
                       "replacement generation owns replacement intent");
                expect(completion.reply.value == 37u,
                       "replacement reply is published first");
            }
        );
        expect(completions == 1,
               "replacement settles without waiting for old generation");
        expect(!tracker.contains(3u),
               "settled replacement releases its active key");

        complete(old_callback, TestReply{29u});
        tracker.drain(
            [&](Tracker::Completion completion)
            {
                ++completions;
                expect(completion.abandoned,
                       "late old generation retains abandoned disposition");
                expect(completion.context.value == 23u,
                       "late old generation retains cleanup intent");
                expect(completion.reply.value == 29u,
                       "late old reply remains observable for compensation");
            }
        );
        expect(completions == 2,
               "replacement and abandoned generations each settle once");
    }

    {
        Tracker tracker;
        (void)tracker.start(
            4u,
            Intent{31u},
            []
            {
                return Factory::makeImmediate(TestReply{37u});
            }
        );
        expect(tracker.cancel(4u), "queued completion can be cancelled");
        (void)tracker.start(
            4u,
            Intent{41u},
            []
            {
                return Factory::makeImmediate(TestReply{43u});
            }
        );

        int completions = 0;
        tracker.drain(
            [&](Tracker::Completion completion)
            {
                ++completions;
                expect(!completion.abandoned,
                       "cancelled request disposition cannot taint replacement");
                expect(completion.context.value == 41u,
                       "stale serial cannot consume replacement intent");
                expect(completion.reply.value == 43u,
                       "stale serial cannot publish old reply");
            }
        );
        expect(completions == 1,
               "only replacement completion survives serial validation");
    }

    {
        Tracker tracker;
        int issue_count = 0;
        (void)tracker.start(
            5u,
            Intent{47u},
            [&issue_count]
            {
                ++issue_count;
                return Factory::makeImmediate(TestReply{53u});
            }
        );
        const auto duplicate = tracker.start(
            5u,
            Intent{59u},
            [&issue_count]
            {
                ++issue_count;
                return Factory::makeImmediate(TestReply{61u});
            }
        );
        expect(duplicate == lux::ecs::ETrackedRequestStart::DUPLICATE_KEY,
               "duplicate active key is rejected");
        expect(issue_count == 1,
               "duplicate rejection never issues a remote create");
    }

    {
        Tracker tracker;
        int completions = 0;
        (void)tracker.start(
            6u,
            Intent{67u},
            [] { return Factory::makeImmediate(TestReply{71u}); }
        );
        tracker.drain(
            [&](Tracker::Completion)
            {
                ++completions;
                (void)tracker.start(
                    7u,
                    Intent{73u},
                    [] { return Factory::makeImmediate(TestReply{79u}); }
                );
            }
        );
        expect(completions == 1,
               "drain handler cannot self-feed an immediate completion");
        tracker.drain(
            [&](Tracker::Completion) { ++completions; }
        );
        expect(completions == 2,
               "completion created during drain waits for the next safe point");
    }

    {
        Factory::Callback late_callback;
        {
            Tracker tracker;
            (void)tracker.start(
                8u,
                Intent{83u},
                [&late_callback]
                {
                    auto issued = Factory::make();
                    late_callback = std::move(issued.callback);
                    return std::move(issued.request);
                }
            );
        }
        complete(late_callback, TestReply{89u});
        expect(true,
               "tracker destruction detaches callback before completion storage");
    }

    {
        auto channel = lux::render::RenderFrameChannel<>::create();
        auto sync = std::make_shared<lux::render::RenderChannelSync>();
        lux::render::RenderFrameSession session(std::move(channel), std::move(sync));
        const lux::render::FeatureOpIds<lux::render::MissingFeatureOp> ids;

        auto request = lux::render::sendWithReply<lux::render::MissingFeatureOp>(
            session,
            ids,
            lux::render::MissingFeaturePayload{47u}
        );

        expect(request.valid(), "missing feature returns a stateful request");
        expect(request.isReady(), "missing feature request settles immediately");
        expect(request.failed(), "missing feature request is a dispatch failure");
        expect(
            lux::render::isError<lux::render::err::comm::FeatureOperationUnavailable>(
                request.error()
            ),
            "missing feature request carries the structured operation error"
        );

        int continuations = 0;
        request.then(
            [&](const lux::render::MissingFeatureReply&)
            {
                ++continuations;
            }
        );
        expect(continuations == 1,
               "missing feature continuation runs synchronously exactly once");
    }

    {
        std::string diagnostic;
        lux::ecs::setRenderBridgeDiagnosticSink(
            [&diagnostic](std::string_view message)
            {
                diagnostic.assign(message);
            }
        );

        const auto recovery = lux::ecs::reportRenderBridgeFailure(
            "request-lifecycle-probe",
            "missing feature operation",
            lux::render::renderError<
                lux::render::err::comm::FeatureOperationUnavailable>()
        );
        expect(recovery == lux::render::ERecovery::Bug,
               "bridge failure helper preserves structured recovery policy");
        expect(
            diagnostic.find("comm.feature_operation_unavailable") !=
                std::string::npos,
            "bridge failure helper formats the stable error name"
        );

        // The sink is process-level and captures test-local storage.
        lux::ecs::setRenderBridgeDiagnosticSink({});
    }

    if (failures != 0)
    {
        std::cerr << failures << " render request lifecycle checks failed\n";
        return 1;
    }
    std::cout << "render request lifecycle checks passed\n";
    return 0;
}
