#include <lux/engine/editor/rendering/detail/ReplyPump.hpp>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace lux::render;
namespace
{
    template <class Channel> void publish(Channel &channel, RequestId request, RenderError error = {})
    {
        auto *packet = channel.responses.tryBeginWrite();
        assert(packet);
        packet->clear_keep_capacity();
        const GenericOkReply value{};
        const CommandFailedReply failure{0, error};
        const bool failed = !error.ok();
        const auto bytes = failed ? sizeof(failure) : sizeof(value);
        packet->payload.resize(bytes);
        std::memcpy(packet->payload.data(), failed ? static_cast<const void *>(&failure) : &value, bytes);
        packet->replies.push_back(ReplyRecord{.type_id = failed ? kReplyCommandFailedTypeId : type_ids::ReplyGenericOk,
                                              .payload_size = static_cast<std::uint32_t>(bytes),
                                              .request_id = request});
        assert(channel.responses.publishWrite());
    }
} // namespace
int main()
{
    auto control_channel = RenderControlChannel<>::create(16);
    auto program_channel = RenderProgramChannel<>::create(2);
    auto upload_channel = RenderUploadChannel<>::create(16);
    auto sync = std::make_shared<RenderChannelSync>();
    RenderControlSession control(control_channel, sync);
    RenderProgramSession program(program_channel, sync);
    RenderUploadSession upload(upload_channel, sync);
    std::vector<unsigned> observed;
    observed.reserve(32);
    std::array<std::array<RenderRequest<GenericOkReply>, 2>, 3> requests;
    assert(program.beginFrame());
    for (unsigned index = 0; index < 2; ++index)
    {
        RemoveViewPayload payload{{0, 1}, {index, 1}};
        requests[0][index] = control.removeView(payload.scene_id, payload.view);
        auto pending = RenderRequestFactory<GenericOkReply>::make();
        const auto id = program.builder().pushWithReply(opcodes::CommandOp, type_ids::RemoveView, payload,
                                                        std::move(pending.callback));
        RenderRequestFactory<GenericOkReply>::bindRequestId(pending.request, id);
        requests[1][index] = std::move(pending.request);
        auto submitted = upload.trySubmit<GenericOkReply>(
            [&](auto &builder, auto callback)
            { builder.pushWithReply(opcodes::CommandOp, type_ids::RemoveView, payload, std::move(callback)); });
        assert(submitted);
        requests[2][index] = std::move(*submitted);
        // Exercise actual client admission and response rings, without claiming GPU execution of these packets.
        OperationPacket<> operation;
        assert(upload_channel->requests.tryPop(operation) == lux::cxx::EQueuePopResult::VALUE);
        RenderRequestFactory<GenericOkReply>::bindRequestId(requests[2][index], operation.requestId());
        upload_channel->releaseBytes(operation.accountedBytes());
        for (unsigned lane = 0; lane < 3; ++lane)
            assert(
                requests[lane][index].then([&, lane, index](const auto &) { observed.push_back(lane * 10 + index); }));
        const auto error = index == 1 ? renderError<err::scene::NotFound>() : RenderError{};
        publish(*control_channel, requests[0][index].requestId(), error);
        publish(*program_channel, requests[1][index].requestId(), error);
        publish(*upload_channel, requests[2][index].requestId(), error);
    }
    std::size_t next{};
    const auto pump = [&](std::size_t budget)
    { return lux::editor::rendering::detail::pumpRendererReplies(control, program, upload, next, budget); };
    assert(pump(0) == 0 && observed.empty() && next == 0);
    assert(control_channel->responses.pendingFrames() == 2 && upload_channel->responses.pendingFrames() == 2);
    assert(pump(1) == 1 && observed == std::vector<unsigned>{0});
    assert(control_channel->responses.pendingFrames() == 1 && program_channel->responses.pendingFrames() == 2);
    assert(pump(1) == 1 && observed == (std::vector<unsigned>{0, 10}));
    assert(pump(1) == 1 && observed == (std::vector<unsigned>{0, 10, 20}));
    // Keep Control busy with an unmatched envelope while other lanes' failure replies wait.
    publish(*control_channel, kInvalidRequestId);
    assert(pump(2) == 2 && observed == (std::vector<unsigned>{0, 10, 20, 1, 11}));
    sync->requestStop();
    assert(pump(1) == 1 && observed.back() == 21);
    for (auto &lane : requests)
    {
        assert(lane[0].isReady() && !lane[0].failed());
        assert(lane[1].isReady() && lane[1].failed());
    }
    assert(pump(1) == 1 && observed.size() == 6);
    assert(pump(4) == 0 && observed.size() == 6);
    assert(control_channel->responses.pendingFrames() == 0 && program_channel->responses.pendingFrames() == 0 &&
           upload_channel->responses.pendingFrames() == 0);
    std::printf("reply budget PASS envelopes=7 callbacks=6 failures=3 unmatched=1 order=0,10,20,1,11,21\n");
    {
        auto hot_control = RenderControlChannel<>::create(4);
        auto hot_program = RenderProgramChannel<>::create(2);
        auto hot_upload = RenderUploadChannel<>::create(4);
        auto hot_sync = std::make_shared<RenderChannelSync>();
        RenderControlSession c(hot_control, hot_sync);
        RenderProgramSession p(hot_program, hot_sync);
        RenderUploadSession u(hot_upload, hot_sync);
        std::size_t cursor{};
        std::array<unsigned, 3> consumed{};
        publish(*hot_control, kInvalidRequestId);
        publish(*hot_program, kInvalidRequestId);
        publish(*hot_upload, kInvalidRequestId);
        for (unsigned step = 0; step != 60; ++step)
        {
            assert(lux::editor::rendering::detail::pumpRendererReplies(c, p, u, cursor, 1) == 1);
            const auto lane = step % 3;
            assert(hot_control->responses.pendingFrames() == (lane == 0 ? 0 : 1));
            assert(hot_program->responses.pendingFrames() == (lane == 1 ? 0 : 1));
            assert(hot_upload->responses.pendingFrames() == (lane == 2 ? 0 : 1));
            ++consumed[lane];
            if (lane == 0)
                publish(*hot_control, kInvalidRequestId);
            else if (lane == 1)
                publish(*hot_program, kInvalidRequestId);
            else
                publish(*hot_upload, kInvalidRequestId);
        }
        assert((consumed == std::array<unsigned, 3>{20, 20, 20}));
        assert(lux::editor::rendering::detail::pumpRendererReplies(c, p, u, cursor, 2) == 2);
        assert(hot_upload->responses.pendingFrames() == 1);
        assert(lux::editor::rendering::detail::pumpRendererReplies(c, p, u, cursor, 2) == 1);
        auto *batch = hot_program->responses.tryBeginWrite();
        assert(batch);
        batch->clear_keep_capacity();
        // A packet is atomic at the existing pump boundary; records are not a second queue.
        batch->payload.resize(sizeof(GenericOkReply));
        batch->replies.assign(64, ReplyRecord{.type_id = type_ids::ReplyGenericOk,
                                              .payload_size = sizeof(GenericOkReply),
                                              .request_id = kInvalidRequestId});
        assert(hot_program->responses.publishWrite());
        publish(*hot_program, kInvalidRequestId);
        assert(lux::editor::rendering::detail::pumpRendererReplies(c, p, u, cursor, 1) == 1);
        assert(hot_program->responses.pendingFrames() == 1 && p.unroutedUnsolicitedReplies() == 85);
        assert(lux::editor::rendering::detail::pumpRendererReplies(c, p, u, cursor, 1) == 1);
        assert(p.unroutedUnsolicitedReplies() == 86);
        std::printf("reply budget sustained PASS pumps=60 lane_consumption=20,20,20 batch_records=64 batch_budget=1\n");
    }
}
