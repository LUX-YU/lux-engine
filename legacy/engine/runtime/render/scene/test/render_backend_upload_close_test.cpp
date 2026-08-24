#include <lux/engine/runtime/execution/AsyncRuntime.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeBuilder.hpp>
#include <lux/engine/runtime/render/scene/AsyncRenderUploadService.hpp>
#include <lux/engine/runtime/render/backend_host/RenderBackendHost.hpp>
#include <lux/engine/runtime/render/scene/testing/AsyncTestServices.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace
{
    void countAdmissionRelease(void* opaque) noexcept
    {
        static_cast<std::atomic<std::uint32_t>*>(opaque)->fetch_add(
            1u,
            std::memory_order_relaxed);
    }
}

int main()
{
    auto admission_releases =
        std::make_shared<std::atomic<std::uint32_t>>(0u);
    {
        lux::runtime::SubmitRenderUploadAdmission first{
            admission_releases,
            &countAdmissionRelease};
        auto queued_then_rejected = std::move(first);
        (void)queued_then_rejected;
    }
    {
        lux::runtime::SubmitRenderUploadAdmission dispatched{
            admission_releases,
            &countAdmissionRelease};
        dispatched.disarm();
    }
    if (admission_releases->load(std::memory_order_relaxed) != 1u)
    {
        std::fputs(
            "FAIL: queued upload admission was not released exactly once\n",
            stderr);
        return 1;
    }

    lux::runtime::RenderBackendHost<> host;
    lux::runtime::RenderBackendHost<>::Config config{};
    if (!host.start(std::move(config)))
    {
        std::puts("SKIP: Vulkan render backend unavailable");
        return 0;
    }

    lux::exec::AsyncRuntimeBuilder async_builder;
    auto upload_service_result =
        lux::runtime::AsyncRenderUploadService::addTo(async_builder);
    auto async_plan = std::move(async_builder).compile();
    if (!upload_service_result || !async_plan)
    {
        std::fputs("FAIL: upload async feature assembly failed\n", stderr);
        (void)host.stop();
        return 1;
    }

    lux::exec::AsyncRuntime async(
        std::move(*async_plan),
        lux::exec::AsyncRuntimeConfig{
            .blocking_io_threads = 1u,
            .background_cpu_concurrency = 1u});
    auto upload_service = std::move(*upload_service_result);
    if (!upload_service.bind(async, host.uploadSession(), host.sync()))
    {
        std::fputs("FAIL: upload coordinator bind failed\n", stderr);
        lux::runtime::testing::detail::closeRuntime(async);
        (void)host.stop();
        return 1;
    }

    constexpr std::int32_t kWidth = 64;
    constexpr std::int32_t kHeight = 64;
    constexpr std::uint64_t kPixelBytes =
        static_cast<std::uint64_t>(kWidth * kHeight * 4);
    std::vector<std::byte> pixels(
        static_cast<std::size_t>(kWidth * kHeight * 4),
        std::byte{0x39}
    );
    auto submitted = upload_service.client().tryCreateTexture2D(
        lux::cxx::SharedBytes<>::copyOf(pixels),
        kWidth,
        kHeight,
        4,
        lux::render::EPixelFormat::RGBA8_UNORM,
        true
    );
    if (!submitted)
    {
        std::fprintf(
            stderr,
            "FAIL: upload admission rejected (%u)\n",
            static_cast<unsigned>(submitted.error())
        );
        lux::runtime::testing::detail::closeUpload(upload_service, async);
        if (upload_service.report().clean)
            upload_service.unbind();
        lux::runtime::testing::detail::closeRuntime(async);
        (void)host.stop();
        return 1;
    }
    auto submitted_copy = upload_service.client().tryCreateTexture2DCopy(
        std::span<const std::byte>{pixels.data(), pixels.size()},
        kWidth,
        kHeight,
        4,
        lux::render::EPixelFormat::RGBA8_UNORM,
        false
    );
    if (!submitted_copy)
    {
        std::fprintf(
            stderr,
            "FAIL: explicit copy upload admission rejected (%u)\n",
            static_cast<unsigned>(submitted_copy.error())
        );
        lux::runtime::testing::detail::closeUpload(upload_service, async);
        if (upload_service.report().clean)
            upload_service.unbind();
        lux::runtime::testing::detail::closeRuntime(async);
        (void)host.stop();
        return 1;
    }
    const auto payload_stats = upload_service.client().statistics();
    if (payload_stats.submitted_packets != 2u ||
        payload_stats.payload_shared_bytes != kPixelBytes ||
        payload_stats.payload_copied_bytes != kPixelBytes)
    {
        std::fprintf(
            stderr,
            "FAIL: shared upload accounting packets=%llu shared=%llu "
            "copied=%llu\n",
            static_cast<unsigned long long>(
                payload_stats.submitted_packets),
            static_cast<unsigned long long>(
                payload_stats.payload_shared_bytes),
            static_cast<unsigned long long>(
                payload_stats.payload_copied_bytes));
        lux::runtime::testing::detail::closeUpload(upload_service, async);
        if (upload_service.report().clean)
            upload_service.unbind();
        lux::runtime::testing::detail::closeRuntime(async);
        (void)host.stop();
        return 1;
    }

    // No FrameProgram is ever opened or submitted. Closing immediately must
    // let the coordinator drain the accepted upload and publish exactly one
    // terminal reply before the render/transfer owners are stopped.
    std::vector<std::byte>{}.swap(pixels);
    lux::runtime::testing::detail::closeUpload(upload_service, async);
    if (!submitted->isReady() || !submitted_copy->isReady() ||
        !upload_service.report().clean)
    {
        const auto upload_report = upload_service.report();
        std::fprintf(
            stderr,
            "FAIL: upload coordinator close was not clean (%zu/%zu)\n",
            upload_report.pending_backpressure,
            upload_report.active_replies);
        (void)host.stop();
        return 2;
    }
    upload_service.unbind();
    lux::runtime::testing::detail::closeRuntime(async);

    const auto report = host.stop();
    if (!report.clean() || report.uploads.accepted != 2 ||
        report.uploads.terminal_ready + report.uploads.terminal_failed != 2 ||
        report.uploads.active != 0 || report.upload_queue_high_water == 0 ||
        report.upload_payload_high_water == 0 ||
        report.uploads.staging_copied_bytes != kPixelBytes * 2u)
    {
        std::fprintf(
            stderr,
            "FAIL: dirty upload close: accepted=%llu ready=%llu failed=%llu "
            "active=%zu stale=%llu duplicate=%llu pending=%zu/%zu/%zu/%zu/"
            "%zu/%zu high_water=%zu/%zu staging=%llu\n",
            static_cast<unsigned long long>(report.uploads.accepted),
            static_cast<unsigned long long>(report.uploads.terminal_ready),
            static_cast<unsigned long long>(report.uploads.terminal_failed),
            report.uploads.active,
            static_cast<unsigned long long>(report.uploads.stale_result),
            static_cast<unsigned long long>(
                report.uploads.duplicate_terminal),
            report.pending_frame_requests,
            report.pending_frame_replies,
            report.pending_control_requests,
            report.pending_control_replies,
            report.pending_upload_requests,
            report.pending_upload_replies,
            report.upload_queue_high_water,
            report.upload_payload_high_water,
            static_cast<unsigned long long>(
                report.uploads.staging_copied_bytes)
        );
        return 3;
    }
    if (!submitted->isReady() || !submitted_copy->isReady())
    {
        std::fputs(
            "FAIL: backend close did not settle the accepted request\n",
            stderr
        );
        return 4;
    }

    std::puts("render backend upload close: PASS");
    return 0;
}
