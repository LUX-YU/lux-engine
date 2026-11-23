#pragma once

#include <lux/engine/render/device/SubmissionQueue.hpp>
#include "FixtureCommands.hpp"
#include "Support.hpp"
#include <array>
#include <cstring>

namespace foundation_test
{
    template <std::size_t Count = 9>
    std::array<std::uint32_t, Count> readBufferWords(
        DeviceSession& session, nri::Buffer& input,
        nri::AccessStage before = {nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY}
    )
    {
        constexpr auto size = sizeof(std::array<std::uint32_t, Count>);
        nri::Buffer* read = nullptr;
        const auto& core = session.nriCore();
        CHECK(
            core.CreatePlacedBuffer(session.device(), NriHostReadbackHeap, {size, 0, nri::BufferUsageBits::NONE}, read) ==
            nri::Result::SUCCESS
        );
        CHECK(submitFixtureCommands(session, session.queue(),
            [&](nri::CommandBuffer& command, std::uint32_t) noexcept -> RenderResult<void>
            {
                const nri::BufferBarrierDesc transition{
                    &input,
                    before,
                    {nri::AccessBits::COPY_SOURCE, nri::StageBits::COPY}
                };
                core.CmdBarrier(command, {nullptr, 0, &transition, 1});
                core.CmdCopyBuffer(command, *read, 0, input, 0, size);
                return {};
            }
        ));
        CHECK(session.drain());
        const void* bytes = core.MapBuffer(*read, 0, size);
        CHECK(bytes);
        CHECK(session.invalidateMappedRange(*read, 0, size) == nri::Result::SUCCESS);
        std::array<std::uint32_t, Count> result{};
        std::memcpy(result.data(), bytes, size);
        core.UnmapBuffer(*read);
        core.DestroyBuffer(read);
        return result;
    }
} // namespace foundation_test
