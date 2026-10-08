#include <lux/engine/function/render/client/RenderProtocol.hpp>
#include <lux/engine/function/render/client/RenderRequest.hpp>

#include <cassert>
#include <cstdio>
#include <cstring>

namespace
{
    using namespace lux::render;
    using Factory = TRenderRequestFactory<SceneCreatedReply>;
    using Request = TRenderRequest<SceneCreatedReply>;

    bool sameError(RenderError first, RenderError second) noexcept
    {
        return first.type == second.type && first.args == second.args;
    }

    template <typename T> ReplyRecord packet(TReplyPacket<>& destination, const T& value, TypeId type = 0)
    {
        destination.payload.resize(sizeof(T));
        std::memcpy(destination.payload.data(), &value, sizeof(T));
        return {.type_id = type, .payload_offset = 0, .payload_size = sizeof(T)};
    }
} // namespace

int main()
{
    const auto reason = renderError<err::comm::PayloadOutOfBounds>(71, 93);
    Request invalid;
    assert(!invalid.valid() && !invalid.isReady() && !invalid.failed());
    assert(!invalid.tryResult());
    assert(!invalid.then([](const Request::Outcome&) noexcept { std::terminate(); }));

    auto pending = Factory::make();
    assert(pending.request.valid() && !pending.request.isReady() && !pending.request.failed());
    assert(pending.request.error().ok());
    assert(pending.request.tryResult().error().type == renderError<err::comm::RequestNotReady>().type);
    Factory::bindRequestId(pending.request, 23);
    unsigned delivered{};
    assert(pending.request.then(
        [&](const Request::Outcome& result) noexcept
        {
            ++delivered;
            assert(!result && sameError(result.error(), reason));
            assert(pending.request.isReady() && pending.request.failed());
            assert(pending.request.requestId() == 23);
            // Terminal publication precedes callbacks. Repeated delivery cannot overwrite it.
            assert(pending.callback.settleFailure(renderError<err::comm::ChannelStopping>()));
            assert(sameError(result.error(), reason));
        }
    ));
    assert(pending.callback.settleFailure(reason));
    assert(delivered == 1 && sameError(pending.request.tryResult().error(), reason));
    assert(pending.request.then(
        [&](const Request::Outcome& result) noexcept
        {
            ++delivered;
            assert(!result && sameError(result.error(), reason));
        }
    ));
    assert(delivered == 2);

    auto immediate = Factory::makeImmediateFailure(reason);
    assert(immediate.then(
        [&](const Request::Outcome& result) noexcept
        {
            immediate = {}; // The synchronous borrow survives destruction of its sole request owner.
            assert(!result && sameError(result.error(), reason));
        }
    ));

    auto dying = Factory::make();
    assert(dying.request.then(
        [&](const Request::Outcome& result) noexcept
        {
            dying.request = {};
            dying.callback = {}; // Including destruction of the currently invoked callback wrapper.
            assert(!result && sameError(result.error(), reason));
        }
    ));
    assert(dying.callback.settleFailure(reason));

    auto cancelled = Factory::make();
    assert(cancelled.request.then([](const Request::Outcome&) noexcept { std::terminate(); }));
    cancelled.request.cancel();
    assert(cancelled.callback.settleFailure(reason));
    assert(cancelled.request.isReady() && cancelled.request.failed());
    assert(sameError(cancelled.request.error(), reason));

    auto scoped = Factory::make();
    auto retained = scoped.request;
    {
        TScopedRenderRequest<SceneCreatedReply> observer{std::move(scoped.request)};
        assert(observer.then([](const Request::Outcome&) noexcept { std::terminate(); }));
    }
    assert(scoped.callback.settleFailure(reason));
    assert(retained.isReady() && retained.failed());

    for (const bool adopt : {false, true})
    {
        TReplyPacket<> bytes;
        SceneCreatedReply value{};
        value.error = reason; // A decoded domain failure is still a valid transport payload.
        const auto record = packet(bytes, value);
        auto success = Factory::make();
        unsigned calls{};
        assert(success.request.then(
            [&](const Request::Outcome& result) noexcept
            {
                ++calls;
                assert(result && sameError(result->error, reason));
            }
        ));
        if (adopt)
        {
            auto adoption = success.callback.prepareMainAdoption(bytes, record);
            assert(adoption && !success.request.isReady());
            (*adoption)();
        }
        else
        {
            success.callback(bytes, record);
        }
        assert(calls == 1 && success.request.isReady() && !success.request.failed());
        assert(sameError(success.request.tryResult()->get().error, reason));

        const auto failure_record = packet(bytes, CommandFailedReply{.error = reason}, kReplyCommandFailedTypeId);
        auto failed = Factory::make();
        assert(failed.request.then(
            [&](const Request::Outcome& result) noexcept
            {
                ++calls;
                assert(!result && sameError(result.error(), reason));
            }
        ));
        if (adopt)
        {
            auto adoption = failed.callback.prepareMainAdoption(bytes, failure_record);
            assert(adoption && !failed.request.isReady());
            (*adoption)();
        }
        else
        {
            failed.callback(bytes, failure_record);
        }
        assert(calls == 2 && failed.request.failed());

        auto malformed = Factory::make();
        const ReplyRecord bad{.payload_offset = 71, .payload_size = 93};
        if (adopt)
        {
            auto adoption = malformed.callback.prepareMainAdoption({}, bad);
            assert(!adoption && sameError(adoption.error(), reason));
            assert(!malformed.request.isReady());
            assert(malformed.callback.settleFailure(adoption.error()));
        }
        else
        {
            malformed.callback({}, bad);
        }
        assert(malformed.request.failed() && sameError(malformed.request.error(), reason));
    }

    auto success = Factory::makeImmediate({});
    assert(success.isReady() && !success.failed() && success.tryResult());
    std::puts("PASS RenderRequest: pending/outcome, exact errors, adoption, cancellation, reentry and owner cleanup");
}
