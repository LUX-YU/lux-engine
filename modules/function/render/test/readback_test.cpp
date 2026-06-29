// ============================================================================
//  readback_test.cpp
//
//  Exercises RenderSession::readbackView (GPU->CPU offscreen-view color
//  readback) end-to-end:
//    1. bring up the render server (window-attached) + a session
//    2. create a scene + a 64x64 OFFSCREEN view (no swapchain binding)
//    3. render a few frames so the view's color image is cleared + all
//       frames-in-flight slots hold content
//    4. readbackView into a CPU buffer
//    5. assert status / dimensions / byte count, and that the image is a
//       single uniform colour (a cleared offscreen target); print the first
//       pixel (BGRA8) for eyeballing.
//
//  Mirrors game_loop_test's windowed bring-up and pumps window events while
//  awaiting replies (a blocking syncCall without event pumping deadlocks the
//  swapchain). Self-checking: 0 = PASS, 1 = FAIL, 0 (skip) if no Vulkan.
// ============================================================================

#include <lux/engine/render/comm/client/RenderSession.hpp>
#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/render/comm/RenderProtocol.hpp>

#include <lux/engine/window/LuxWindow.hpp>
#include <GLFW/glfw3.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

using namespace lux::render;

static std::vector<const char*> getVulkanExtensions()
{
    glfwInit();
    uint32_t count = 0;
    const char** exts = glfwGetRequiredInstanceExtensions(&count);
    std::vector<const char*> result;
    result.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
        result.emplace_back(exts[i]);
    return result;
}

// Submit the current frame BLOCKING (guarantees publication — a non-blocking
// submit silently leaves the frame staged when the SPSC ring is full, which
// desyncs the begin/submit state machine and drops later commands), then pump
// window events + replies until @p req resolves. Leaves a fresh frame begun.
template <class T>
static T await(RenderSession& s, lux::window::LuxWindow& w, RenderRequest<T> req)
{
    s.submitFrame(/*blocking=*/true);
    while (!req.isReady())
    {
        w.pollEvents();
        s.waitAndPumpReplies();
    }
    T r = req.result();
    s.beginFrame({});
    return r;
}

int main()
{
    std::cout << std::unitbuf; // flush every <<, so progress survives a hang/kill
    std::cout << "=== Render readbackView Test ===\n";

    constexpr uint32_t W = 64;
    constexpr uint32_t H = 64;

    auto channel = RenderProgramChannel<>::create();
    auto sync    = std::make_shared<RenderChannelSync>();
    auto exts    = getVulkanExtensions();

    lux::window::LuxWindow window(static_cast<int>(W), static_cast<int>(H), "readback_test");
    std::cout << "[1] window created\n";

    std::atomic<bool> ready{false};
    std::atomic<bool> failed{false};

    std::thread server_thread([&]
    {
        GeneralRenderServer server(channel, sync);
        ServerConfig cfg;
        cfg.instance_extensions = exts;
        if (auto r = server.init(std::move(cfg)); !r)
        {
            std::cerr << "[Server] init failed: " << r.error().message() << "\n";
            failed.store(true, std::memory_order_release);
            ready.store(true, std::memory_order_release);
            return;
        }
        if (auto r = server.attachToWindow(window); !r)
        {
            std::cerr << "[Server] attachToWindow failed: " << r.error().message() << "\n";
            failed.store(true, std::memory_order_release);
            ready.store(true, std::memory_order_release);
            return;
        }
        ready.store(true, std::memory_order_release);
        try
        {
            int ticks = 0;
            while (server.tick()) { ++ticks; }
            std::cerr << "[Server] tick loop exited normally after " << ticks << " ticks\n";
        }
        catch (const std::exception& e) { std::cerr << "[Server] tick threw: " << e.what() << "\n"; }
        catch (...)                     { std::cerr << "[Server] tick threw (unknown)\n"; }
    });

    while (!ready.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    if (failed.load(std::memory_order_acquire))
    {
        std::cerr << "Server init failed (no Vulkan device?). Skipping test.\n";
        sync->requestStop();
        server_thread.join();
        return 0; // skip, not fail
    }
    std::cout << "[2] server ready\n";

    RenderSession session(channel, sync);

    session.beginFrame({});
    const auto scene = await(session, window, session.createScene("ReadbackTest"));
    std::cout << "[3] scene created id=" << scene.scene_id.index << "\n";
    await(session, window, session.setActiveScene(scene.scene_id, true));
    std::cout << "[4] scene activated\n";
    const auto view = await(session, window,
                            session.addView(scene.scene_id, {W, H}, "ReadbackView"));
    std::cout << "[5] view added id=" << view.view.index << "\n";

    // Render several frames (BLOCKING submit — see await() — so the begin/submit
    // state machine never desyncs) so the offscreen color image is cleared and
    // every frame-in-flight slot holds the same content.
    for (int i = 0; i < 8; ++i)
    {
        window.pollEvents();
        session.submitFrame(/*blocking=*/true);
        session.waitAndPumpReplies();
        session.beginFrame({});
    }
    std::cout << "[6] rendered warm-up frames\n";

    std::vector<std::uint8_t> px(static_cast<std::size_t>(W) * H * 4, 0);
    const auto rb = await(session, window,
        session.readbackView(scene.scene_id, view.view, px.data(), px.size()));
    std::cout << "[7] readback returned\n";

    std::cout << "readback: status=" << rb.status
              << " size=" << rb.width << "x" << rb.height
              << " bpp=" << rb.bytes_per_pixel
              << " bytes_written=" << rb.bytes_written
              << " vkformat=" << rb.format << "\n";

    int fails = 0;
    auto check = [&](bool cond, const char* name)
    {
        std::cout << (cond ? "  [PASS] " : "  [FAIL] ") << name << "\n";
        if (!cond) ++fails;
    };

    check(rb.status == 0, "status == 0 (success)");
    check(rb.width == W && rb.height == H, "dimensions match the view");
    check(rb.bytes_written == static_cast<std::uint64_t>(W) * H * 4, "bytes_written == W*H*4");

    bool uniform = (rb.status == 0);
    for (std::size_t i = 4; uniform && i + 4 <= px.size(); i += 4)
    {
        if (px[i] != px[0] || px[i + 1] != px[1] ||
            px[i + 2] != px[2] || px[i + 3] != px[3])
            uniform = false;
    }
    check(uniform, "image is a single uniform colour (cleared)");

    std::cout << "  first pixel (BGRA8) = ("
              << static_cast<int>(px[0]) << ", " << static_cast<int>(px[1]) << ", "
              << static_cast<int>(px[2]) << ", " << static_cast<int>(px[3]) << ")\n";

    // ── Async (deferred) readback of the same static view ───────────────────
    // readbackViewAsync returns immediately; the server settles N ticks, submits
    // the copy WITHOUT blocking its thread, polls the fence, then sends a
    // DEFERRED reply matched by request_id. We drive frames (each blocking
    // submit advances one server tick) until the request resolves — never
    // blocking on a fence, which is the whole point of the async path.
    std::vector<std::uint8_t> px_async(px.size(), 0);
    auto rb_req = session.readbackViewAsync(
        scene.scene_id, view.view, px_async.data(), px_async.size(), /*settle_frames=*/3);

    int frames = 0;
    while (!rb_req.isReady() && frames++ < 300)
    {
        window.pollEvents();
        session.submitFrame(/*blocking=*/true);   // each submit drives one server tick
        session.waitAndPumpReplies();
        session.beginFrame({});
    }
    const bool async_ready = rb_req.isReady();
    std::cout << "[8] async readback " << (async_ready ? "resolved" : "TIMED OUT")
              << " after " << frames << " frames\n";
    check(async_ready, "async request resolved (deferred reply arrived)");

    if (async_ready)
    {
        const auto rb_async = rb_req.result();
        std::cout << "async readback: status=" << rb_async.status
                  << " size=" << rb_async.width << "x" << rb_async.height
                  << " bytes_written=" << rb_async.bytes_written << "\n";
        check(rb_async.status == 0, "async status == 0 (success)");
        check(rb_async.width == W && rb_async.height == H, "async dimensions match the view");
        check(rb_async.bytes_written == static_cast<std::uint64_t>(W) * H * 4,
              "async bytes_written == W*H*4");
        check(px_async == px, "async readback bytes match the sync readback (identical static view)");
    }

    sync->requestStop();
    server_thread.join();

    std::cout << "=== readback_test " << (fails == 0 ? "PASSED" : "FAILED")
              << " (fails=" << fails << ") ===\n";
    return fails == 0 ? 0 : 1;
}
