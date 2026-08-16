#include "DeviceRenderFixture.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

int main()
{
    lux::rendertest::DeviceRenderFixture fixture(
        32, 32, "upload_without_frame_test");
    if (!fixture.ok())
    {
        std::printf("SKIP: Vulkan device unavailable\n");
        return 0;
    }

    constexpr std::int32_t kWidth = 8;
    constexpr std::int32_t kHeight = 8;
    std::vector<std::byte> pixels(
        static_cast<std::size_t>(kWidth * kHeight * 4),
        std::byte{0x7f});

    auto submitted = fixture.upload().tryCreateTexture2D(
        lux::cxx::SharedBytes<>::copyOf(pixels),
        kWidth,
        kHeight,
        4,
        lux::render::EPixelFormat::RGBA8_UNORM,
        true);
    if (!submitted)
    {
        std::fprintf(stderr, "upload admission failed: %u\n",
            static_cast<unsigned>(submitted.error()));
        return 1;
    }

    // OperationPacket owns every byte reachable by the transfer thread.
    // Releasing the caller buffer before the server consumes the packet is a
    // deterministic regression guard against borrowed-span upload contracts.
    std::vector<std::byte>{}.swap(pixels);

    // DeviceRenderFixture has an OPEN client-side frame for ordinary tests,
    // but no FrameProgram is submitted anywhere in this test.
    const auto reply = fixture.awaitUpload(std::move(*submitted));
    if (reply.status != 0 || reply.handle.isNull())
    {
        std::fprintf(stderr,
            "upload did not reach READY without a frame (status=%u)\n",
            reply.status);
        return 2;
    }

    fixture.control().destroyTexture(reply.handle);
    return 0;
}
