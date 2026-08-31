#include <lux/engine/function/render/client/RenderClient.hpp>
#include <lux/engine/function/render/client/core/FrameStamp.hpp>
#include <lux/engine/render/comm/server/RenderServer.hpp>

#include <cassert>
#include <cstdint>
#include <memory>

namespace
{
    class TestProgramServer final : public lux::render::RenderServer<>
    {
    public:
        TestProgramServer(
            std::shared_ptr<Channel> channel,
            std::shared_ptr<lux::render::RenderChannelSync> sync
        )
            : RenderServer(std::move(channel), std::move(sync), dispatcher_)
        {
        }

        [[nodiscard]] bool drainOne()
        {
            auto before_execute = [this](const lux::render::RenderProgram<>& program) noexcept {
                if (program.kind == lux::render::ERenderProgramKind::Frame)
                {
                    last_stamp_ = clock_.beginTick(0);
                }
            };
            if (!acquireAndExecute(false, nullptr, before_execute))
            {
                return false;
            }
            return finalizeReplies(false);
        }

        [[nodiscard]] std::uint64_t frameSerial() const noexcept
        {
            return last_stamp_.serial;
        }

    private:
        Dispatcher dispatcher_{};
        lux::render::FrameClock clock_{2};
        lux::render::FrameStamp last_stamp_{0, lux::render::FrameSlot{}, 2, 0};
    };
}

int main()
{
    auto channel = lux::render::RenderProgramChannel<>::create(1);
    auto sync = std::make_shared<lux::render::RenderChannelSync>();
    lux::render::RenderClient<> client{channel, sync};
    TestProgramServer server{channel, sync};

    lux::render::RenderProgram<> update;
    for (std::uint32_t index = 0; index < 100; ++index)
    {
        assert(client.trySubmitPrepared(update));
        assert(server.drainOne());
        client.pumpReplies();
        assert(server.frameSerial() == 0);
        assert(update.kind == lux::render::ERenderProgramKind::StateUpdate);
    }

    lux::render::RenderProgram<> frame;
    frame.kind = lux::render::ERenderProgramKind::Frame;
    assert(client.trySubmitPrepared(frame));
    assert(server.drainOne());
    client.pumpReplies();
    assert(server.frameSerial() == 1);
    assert(frame.kind == lux::render::ERenderProgramKind::StateUpdate);
    return 0;
}
