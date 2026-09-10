#include "../DevelopmentScene.hpp"
#include <lux/engine/scene/ResolvedMeshResources.hpp>
#include <lux/engine/editor/ui/scene/SceneWorkspace.hpp>
#include <lux/engine/editor/rendering/detail/ViewImageLifetime.hpp>
#include <lux/engine/editor/rendering/detail/RendererTestAccess.hpp>
#include <lux/engine/editor/sessions/scene/SceneResourceStatus.hpp>
#include <lux/engine/editor/sessions/scene/detail/SceneTestAccess.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>
#include <lux/engine/window/LuxWindow.hpp>
#include <lux/engine/resource/asset/storage/pak/PakAssetProvider.hpp>
#include <lux/engine/simulation/ecs/Parent.hpp>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
#include <type_traits>
#include "../../../rendering/test/ViewLifetimeTest.hpp"
#if defined(LUX_EDITOR_DIAGNOSTICS)
extern "C" __declspec(dllimport) std::size_t lux_er1_client_allocation_disarm() noexcept;
extern "C" __declspec(dllimport) void lux_er1_function_ui_allocation_fail_after(std::size_t) noexcept;
extern "C" __declspec(dllimport) std::size_t lux_er1_function_ui_allocation_disarm() noexcept;
#endif

namespace
{
    using namespace lux::editor;
    using Clock = std::chrono::steady_clock;
    class GatedProvider final : public lux::asset::IAssetProvider
    {
    public:
        std::shared_ptr<lux::asset::IAssetProvider> source;
        lux::asset::AssetId held;
        bool hold_all{};
        lux::asset::AssetId failed_asset;
        lux::asset::AssetId secondary_held;
        std::atomic<bool> secondary_released{};
        mutable std::atomic<unsigned> entered{}, returned{};
        std::atomic<bool> released{};
        std::optional<lux::asset::AssetId> resolve(std::string_view path) const override
        {
            return source->resolve(path);
        }
        bool contains(const lux::asset::AssetId &id) const override
        {
            return source->contains(id);
        }
        lux::cxx::expected<lux::asset::AssetBlob, lux::asset::EAssetStorageError> open(
            const lux::asset::AssetId &id) const override
        {
            if (hold_all || id == held)
            {
                ++entered;
                while (!released.load(std::memory_order_acquire))
                    released.wait(false, std::memory_order_acquire);
                // secondary_held is set before release(); the acquire above publishes that test configuration.
                if (id == secondary_held)
                {
                    std::fprintf(stderr, "G02 held material tail=%u\n", std::to_integer<unsigned>(id.bytes().back()));
                    while (!secondary_released.load(std::memory_order_acquire))
                        secondary_released.wait(false, std::memory_order_acquire);
                }
                auto result = id == failed_asset
                                  ? lux::cxx::expected<lux::asset::AssetBlob,
                                                       lux::asset::EAssetStorageError>{lux::cxx::unexpected(
                                        lux::asset::EAssetStorageError::IO_FAILURE)}
                                  : source->open(id);
                ++returned;
                return result;
            }
            return source->open(id);
        }
        void enumerate(const std::function<void(const lux::asset::ProviderEntry &)> &visitor) const override
        {
            source->enumerate(visitor);
        }
        std::optional<std::string> pathOf(const lux::asset::AssetId &id) const override
        {
            return source->pathOf(id);
        }
        void release() noexcept
        {
            released.store(true, std::memory_order_release);
            released.notify_all();
        }
    };
    void require(bool success, const char *operation)
    {
        if (success)
            return;
        std::fprintf(stderr, "ER1 GPU FAIL: %s\n", operation);
        std::fflush(stderr);
        std::abort();
    }
    template <class T> void require(const T &result, const char *operation)
    {
        require(static_cast<bool>(result), operation);
    }
    class FailingPane final : public lux::object::Object<FailingPane, lux::ui::Pane>
    {
    public:
        explicit FailingPane(ui::EditorWindow &window, std::string id = "test.foreign.failure")
            : Object(window.uiSession().dispatcherRef(), lux::ui::PaneId{std::move(id)},
                     lux::ui::PaneTypeId{"test.foreign.failure"}, "Foreign preparation failure"),
              window_(window)
        {
        }
        bool fail{true};
        std::size_t calls{};

    private:
        ui::EditorWindow &window_;
        void draw(lux::ui::Frame &frame, lux::ui::PaneDrawContext &) override
        {
            ++calls;
            const auto closed = window_.closeAfterRendererStopped();
            require(!closed && closed.error().code == ui::EWindowError::BUSY, "draw-time close retains window owner");
            auto table = frame.table({lux::ui::WidgetIdView{"failure-scope"}, 2, false, false, false});
            if (fail)
                throw std::bad_alloc{};
            frame.text("Recovered foreign pane");
        }
    };
    struct Readback final
    {
        rendering::ViewImage image;
        std::vector<std::byte> pixels;
        lux::render::RenderRequest<lux::render::ReadbackTargetReply> request;
        // The actual request references both buffer and target. Neither owner is released while pending.
        void start(lux::scene::RenderRuntimeLease &runtime, rendering::ViewImage value)
        {
            require(!request.valid(), "single readback admission");
            image = std::move(value);
            const auto *record = rendering::detail::ViewImageAccess::record(image);
            require(record != nullptr, "readback retains real image record");
            pixels.resize(std::size_t(image.extent.width) * image.extent.height * 4);
            request = runtime.control().readbackTargetAsync(record->version->target, pixels.data(), pixels.size());
            require(request.valid(), "real readback request");
        }
        std::uint64_t finish(const std::filesystem::path &path)
        {
            require(request.isReady(), "readback completion");
            const auto result = request.tryResult();
            require(result, "readback transport");
            const auto &reply = result->get();
            require(reply.status == 0 && reply.bytes_written == pixels.size(), "readback backend and byte count");
            require(reply.width == image.extent.width && reply.height == image.extent.height, "readback extent");
            std::ofstream file(path, std::ios::binary);
            file << "P6\n" << reply.width << ' ' << reply.height << "\n255\n";
            std::uint64_t checksum = 14695981039346656037ULL;
            for (std::size_t i = 0; i < pixels.size(); i += 4)
            {
                const char rgb[]{char(pixels[i + 2]), char(pixels[i + 1]), char(pixels[i])};
                file.write(rgb, 3);
                for (const auto channel : rgb)
                    checksum = (checksum ^ static_cast<unsigned char>(channel)) * 1099511628211ULL;
            }
            require(file.good(), "write raw image evidence");
            std::printf("capture=%s extent=%ux%u checksum=%llu\n", path.string().c_str(), reply.width, reply.height,
                        static_cast<unsigned long long>(checksum));
            std::fflush(stdout);
            request = {};
            image = {};
            return checksum;
        }
    };
    template <class Failure> bool preservesSceneFailure(const Failure &failure, const sessions::SceneFailure &source)
    {
        if constexpr (requires { failure.scene; })
        {
            if (!failure.scene || failure.scene->code != source.code || failure.scene->session != source.session ||
                bool(failure.scene->renderer) != bool(source.renderer))
                return false;
            if (!source.renderer)
                return true;
            const auto &actual = *failure.scene->renderer;
            const auto &expected = *source.renderer;
            return actual.code == expected.code && actual.view == expected.view && actual.request == expected.request &&
                   actual.render_error.type == expected.render_error.type &&
                   actual.render_error.args == expected.render_error.args &&
                   actual.backend_status == expected.backend_status;
        }
        return false;
    }
    class ViewCloseObserver final : public lux::object::Object<ViewCloseObserver>
    {
    public:
        using Object::Object;
        sessions::SceneView *view{};
        ui::SceneWorkspace *workspace{};
        bool preserved{};
        unsigned calls{};
        void changed(const sessions::ViewImageNotice &) noexcept
        {
            ++calls;
            const auto direct = view->beginClose();
            require(!direct && direct.error().code == sessions::ESceneError::BUSY,
                    "G04 SceneView reports real publisher reentry BUSY");
            require(workspace->beginClose(), "G04 Workspace retains close intent");
            const auto propagated = workspace->advanceClose();
            preserved = !propagated && preservesSceneFailure(propagated.error(), direct.error());
            std::printf("G04 close scene_code=%u session=%llu preserved=%u\n", unsigned(direct.error().code),
                        direct.error().session.value, unsigned(preserved));
        }
    };
    class ResourceObserver final : public lux::object::Object<ResourceObserver>
    {
    public:
        using Object::Object;
        sessions::SceneSession *session{};
        unsigned calls{};
        void changed(const sessions::SceneResourceNotice &notice) noexcept
        {
            const auto snapshot = session->readResources();
            require(snapshot && (*snapshot)->session == notice.session &&
                        (*snapshot)->revision == notice.resource_revision,
                    "G02 notification observes the completely published resource snapshot");
            ++calls;
        }
    };
    class ClosingObserver final : public lux::object::Object<ClosingObserver>
    {
    public:
        using Object::Object;
        ui::EditorWindow *window{};
        ui::SceneWorkspace *workspace{};
        sessions::SceneSession *session{};
        unsigned calls{};
        void selected(const sessions::SceneSelectionNotice &notice) noexcept
        {
            ++calls;
            require(window->frameOpen() && session->selection().current == notice.current,
                    "selection publisher remains alive with committed data inside the UI frame");
            require(session->readOutline() && session->historyView(), "callback can query consistent owner state");
            const auto closed = session->beginClose();
            require(!closed && closed.error().code == sessions::ESceneError::BUSY,
                    "recursive Session close cannot release the active publisher");
            require(window->requestClose() && window->closeRequested(), "Window accepts a value close request");
            require(workspace->beginClose(), "Workspace records close intent in selection callback");
            const auto step = workspace->advanceClose();
            require(step && *step == sessions::ECloseProgress::PENDING,
                    "frame keeps Workspace panes, registrations and View alive until safe point");
            require(session->readOutline() && window->frameOpen(), "request does not tear down callback owners");
        }
    };
    template <class T>
    constexpr bool fixedOwner = !std::is_move_constructible_v<T> && !std::is_move_assignable_v<T> &&
                                !std::is_copy_constructible_v<T> && !std::is_copy_assignable_v<T>;
    static_assert(fixedOwner<application::EditorApplication> && fixedOwner<ui::EditorWindow> &&
                  fixedOwner<rendering::EditorRenderer> && fixedOwner<rendering::RenderView> &&
                  fixedOwner<sessions::SceneSession> && fixedOwner<sessions::SceneView> &&
                  fixedOwner<ui::SceneWorkspace>);

    void foreignOwnerCalls(ui::EditorWindow &window, rendering::EditorRenderer &renderer,
                           sessions::SceneSession &session, sessions::SceneView &view, ui::SceneWorkspace &workspace)
    {
        const auto selection = session.selection();
        const auto history = session.historyView();
        const auto before = renderer.statistics();
        require(history, "owner history before foreign thread rejection");
        unsigned rejected{};
        std::thread foreign([&] {
            const auto scene = [&](const auto &result) {
                require(!result && result.error().code == sessions::ESceneError::WRONG_THREAD,
                        "Scene owner rejects foreign thread before mutation");
                ++rejected;
            };
            const auto shell = [&](const auto &result) {
                require(!result, "Window/Workspace rejects foreign thread before native UI or owner mutation");
                if constexpr (requires { result.error().window; })
                    require(result.error().window && result.error().window->code == ui::EWindowError::WRONG_THREAD,
                            "Workspace retains owning Window thread error");
                else
                    require(result.error().code == ui::EWindowError::WRONG_THREAD, "Window thread error");
                ++rejected;
            };
            const auto render = [&](const auto &result) {
                require(!result && result.error().code == rendering::ERendererError::WRONG_THREAD,
                        "Renderer rejects foreign thread before queue or lifecycle mutation");
                ++rejected;
            };
            scene(session.select(std::nullopt));
            scene(session.readOutline());
            scene(session.beginClose());
            scene(session.advanceClose());
            scene(view.resetCamera());
            scene(view.moveCamera({}));
            scene(view.requestExtent({32, 32}));
            scene(view.synchronize());
            scene(view.beginClose());
            scene(view.advanceClose());
            shell(window.collectInput());
            shell(window.requestClose());
            shell(window.closeAfterRendererStopped());
            shell(workspace.activate());
            shell(workspace.updateBeforeFrame());
            shell(workspace.beginClose());
            shell(workspace.advanceClose());
            render(renderer.poll(0));
            render(renderer.beginClose());
            render(renderer.advanceClose());
            render(renderer.joinStopped());
        });
        foreign.join();
        const auto after = renderer.statistics();
        require(rejected == 21 && session.state() == sessions::ESessionState::READY &&
                    session.selection().current == selection.current &&
                    session.historyView()->history.current == history->history.current &&
                    renderer.state() == rendering::ERendererState::READY && before.views == after.views &&
                    before.runtime_leases == after.runtime_leases && !window.closeRequested() && !window.frameOpen(),
                "foreign rejection preserves source owners, selection/history and registrations");
        require(workspace.activate(), "same owner remains usable after all foreign calls");
        std::printf("owner thread PASS rejected=%u retained_window_renderer_session_view_workspace\n", rejected);
    }
} // namespace

int main(int argc, char **argv)
{
    using namespace lux::editor;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    require(argc == 4 || argc == 5, "arguments: asset package, output directory, scene variant, optional recovery pak");
    const std::string_view variant{argv[3]};
    bool failed_view_contract = true;
    bool resource_publication_contract = true;
    bool coordinate_contract = true;
    bool workspace_failure_contract = true;
    bool resolved_source_contract = true;
    const bool workspace_failure_test = variant == "workspace_failure";
    const bool shader_subfailure_test = variant == "shader_subfailure";
    const bool backpressure_close = variant == "resource_backpressure_close";
    const bool resource_backpressure_test = variant == "resource_backpressure" || backpressure_close;
    std::shared_ptr<const sessions::SceneResourceSnapshot> backpressured_resources;
    const bool coordinate_test = variant == "coordinate_256" || variant == "coordinate_1024";
    const double page_size = variant == "coordinate_256" ? 256.0 : 1024.0;
    const Eigen::Vector3d coordinate_offset{256, -256, 1024};
    const bool resource_snapshot_test = variant == "resource_snapshot";
    const bool resource_ready_test = variant == "resource_ready_publication";
    const bool resource_publication_test =
        variant == "resource_publication" || resource_snapshot_test || resource_ready_test;
    std::uint64_t prepared_cycles{};
    const bool retry_test = variant == "retry";
    const bool dynamic_test = variant == "dynamic";
    const bool churn_test = variant == "churn";
    const bool multiple_views_test = variant.starts_with("multiple_");
    const bool multiple_reverse = variant == "multiple_reverse";
    const bool multiple_equal = variant == "multiple_equal";
    const bool multiple_lifecycle = variant == "multiple_lifecycle";
    const bool record_failure_test = variant == "record_failure";
    const bool late_entity_test = variant == "late_entity";
    const bool late_source_test = variant == "late_source";
    const bool late_selection_test = variant == "late_selection";
    const bool partial_failure_test = variant == "partial_late_close";
    const bool late_close_test = variant == "late_close" || partial_failure_test;
    const bool gated_test = late_entity_test || late_source_test || late_close_test || late_selection_test;
#if !defined(LUX_EDITOR_DIAGNOSTICS)
    require(!dynamic_test && !churn_test && !record_failure_test && !late_entity_test && !late_source_test &&
                variant != "factory_failure" && variant != "view_admission" && !resource_publication_test &&
                !workspace_failure_test && !shader_subfailure_test && !resource_backpressure_test &&
                variant != "ui_atlas_failure",
            "requested variant requires the isolated diagnostic build");
#endif
    require(!retry_test || argc == 5, "retry needs a complete recovery package");
    const bool has_mesh = variant != "no_mesh" && variant != "empty";
    const auto output = std::filesystem::path(argv[2]);
    std::filesystem::create_directories(output);
    lux::meta::ReflectionRegistry::initRegistry();
    {
        lux::window::GlfwRuntime platform;
        require(platform.valid(), "GLFW startup");
        lux::object::ObjectMessageQueue messages;
        auto execution = lux::process::ExecutionRuntime::create(
            {2, 64, 64, {64}, lux::process::BlockingSchedulerConfig{partial_failure_test ? 4u : 2u, 64}});
        require(execution, "execution factory");
        lux::asset::AssetVfs vfs;
        auto pak = lux::asset::PakAssetProvider::loadFromFile(argv[1]);
        require(pak, "load actual scene resources");
        auto provider = std::make_shared<GatedProvider>();
        provider->source = *pak;
        provider->hold_all = resource_publication_test;
        if (!gated_test && !resource_publication_test)
            provider->release();
        require(vfs.mount({"/Seed", provider, 0}) != lux::asset::kInvalidMountId, "mount resources");
        auto blocking = execution->blocking();
        require(blocking, "blocking scheduler");
        auto endpoint = lux::process::asset_loading::VfsAssetReadEndpoint::create(vfs.view(), *blocking, {64});
        require(endpoint, "asset endpoint");
        ui::WindowSpec window_config;
        window_config.visible = false;
        auto window_result = ui::EditorWindow::create(messages.dispatcherRef(), window_config);
        require(window_result, "window factory");
        auto window = std::move(*window_result);
        rendering::RendererConfig config;
        config.validation = true;
        if (resource_backpressure_test)
            config.control_capacity = config.upload_capacity = 2;
        if (variant == "view_failure")
            config.diagnostic_capacity = 1;
        config.validation_message_sink = [](std::uint32_t severity, std::string_view text) {
            std::fprintf(stderr, "Vulkan severity=%u %.*s\n", severity, int(text.size()), text.data());
        };
#if defined(LUX_EDITOR_DIAGNOSTICS)
        if (variant == "factory_failure")
        {
            std::size_t failures{};
            for (std::size_t index = 0; index < 512; ++index)
            {
                lux_er1_renderer_allocation_fail_after(index);
                auto attempted = rendering::EditorRenderer::create(window->nativeWindow(), window->uiSession(), config);
                const auto allocations = lux_er1_renderer_allocation_disarm();
                if (attempted)
                {
                    require(allocations == index, "renderer factory actual allocation count");
                    require((*attempted)->beginClose(), "empty renderer begins close");
                    bool complete{};
                    const auto close_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
                    while (!complete && std::chrono::steady_clock::now() < close_deadline)
                    {
                        const auto closed = (*attempted)->advanceClose();
                        require(closed, "empty renderer close progresses");
                        complete = *closed == rendering::ERenderClose::COMPLETE;
                        if (!complete)
                            std::this_thread::sleep_for(std::chrono::milliseconds{1});
                    }
                    require(complete && (*attempted)->joinStopped(), "empty renderer actually joined");
                    break;
                }
                ++failures;
                require(allocations == index + 1 &&
                            attempted.error().code == rendering::ERendererError::ALLOCATION_FAILURE,
                        "actual renderer factory allocation failure preserved");
            }
            require(failures > 12 && failures < 512, "renderer allocation sweep reached successful construction");
            std::printf("Renderer factory actual DLL allocation failures checked: %zu\n", failures);
        }
#endif
#if defined(LUX_EDITOR_DIAGNOSTICS)
        if (variant == "ui_atlas_failure")
        {
            auto *const original_ui = &window->uiSession();
            lux_er1_function_ui_allocation_fail_after(0);
            auto failed = rendering::EditorRenderer::create(window->nativeWindow(), window->uiSession(), config);
            const auto attempts = lux_er1_function_ui_allocation_disarm();
            require(!failed && failed.error().code == rendering::ERendererError::ALLOCATION_FAILURE && attempts == 1 &&
                        &window->uiSession() == original_ui,
                    "actual UI DLL atlas copy failure retains exact Renderer error and UI owner");
            require(window->beginFrame({{1600, 900}, 1.0F / 60, {1, 1}}) && window->drawPanes() &&
                        window->finishFrame(),
                    "Window UI remains usable after rejected Renderer creation");
            std::puts("UI atlas copy: actual DLL allocation rejected; Renderer ALLOCATION_FAILURE; Window retained");
        }
#endif
        auto renderer_result = rendering::EditorRenderer::create(window->nativeWindow(), window->uiSession(), config);
        require(renderer_result, "renderer factory");
        auto renderer = std::move(*renderer_result);
#if defined(LUX_EDITOR_DIAGNOSTICS)
        if (variant == "view_admission")
        {
            const auto close = [&](std::unique_ptr<rendering::RenderView> &view) {
                require(view->beginClose(), "C06 admitted View retains close owner");
                const auto deadline = Clock::now() + std::chrono::seconds{10};
                for (;;)
                {
                    require(Clock::now() < deadline && renderer->poll(64), "C06 finite View closure");
                    const auto closed = view->advanceClose();
                    require(closed, "C06 View close result");
                    if (*closed == rendering::ERenderClose::COMPLETE)
                        break;
                }
                view.reset();
            };
            std::size_t failures{};
            for (std::size_t index = 0; index < 16; ++index)
            {
                const auto before = renderer->statistics();
                lux_er1_renderer_allocation_fail_after(index);
                auto attempted = renderer->openView({1000, 9}, {{0, 0}, true});
                const auto allocations = lux_er1_renderer_allocation_disarm();
                if (attempted)
                {
                    require(allocations == index && renderer->statistics().views == before.views + 1,
                            "C06 View factory sweeps every actual DLL allocation through successful admission");
                    close(*attempted);
                    break;
                }
                ++failures;
                require(attempted.error().code == rendering::ERendererError::ALLOCATION_FAILURE &&
                            allocations == index + 1 && renderer->statistics().views == before.views &&
                            renderer->statistics().render_events == before.render_events,
                        "C06 failed View factory releases partial ownership before any request is published");
            }
            require(failures == 4, "C06 Impl/owner/resources/image-version actual allocation coverage");
            std::vector<std::unique_ptr<rendering::RenderView>> owners;
            owners.reserve(config.view_capacity);
            for (std::size_t index = 0; index < config.view_capacity; ++index)
            {
                auto view = renderer->openView({1000, 9}, {{0, 0}, true});
                require(view, "C06 each configured registration slot admits one owner");
                owners.push_back(std::move(*view));
            }
            lux_er1_renderer_allocation_fail_after(0);
            const auto full = renderer->openView({1000, 9}, {{0, 0}, true});
            const auto allocations = lux_er1_renderer_allocation_disarm();
            require(!full && full.error().code == rendering::ERendererError::CAPACITY && allocations == 0 &&
                        renderer->statistics().views == config.view_capacity,
                    "C06 full registration refuses before allocation or backend publication");
            const auto retired = owners.back()->id();
            close(owners.back());
            auto replacement = renderer->openView({1000, 9}, {{0, 0}, true});
            require(replacement && (*replacement)->id() != retired, "C06 reclaimed slot never reuses View identity");
            owners.back() = std::move(*replacement);
            for (auto &owner : owners)
                close(owner);
            require(renderer->statistics().views == 0 && renderer->statistics().render_events == 0,
                    "C06 capacity/factory failure leaves no View or backend error");
            std::printf("C06 PASS factory_allocations=%zu capacity=%zu full_allocations=0 closed_views=0\n", failures,
                        config.view_capacity);
        }
#endif
        for (const auto size : {0.0, -1.0, (std::numeric_limits<double>::max)(),
                                std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
        {
            const auto before = renderer->statistics();
            const auto rejected = renderer->openView({1000, 9}, {{64, 64}, true, size});
            require(!rejected && rejected.error().code == rendering::ERendererError::INVALID_ARGUMENT &&
                        renderer->statistics().views == before.views &&
                        renderer->statistics().render_events == before.render_events,
                    "G03 invalid camera page size rejected before owner/request admission");
        }
        if (variant == "view_failure")
        {
            // Real backend replies for nonexistent Scene identities, not fabricated diagnostics.
            std::array<std::unique_ptr<rendering::RenderView>, 2> rejected_views;
            for (std::size_t i = 0; i < rejected_views.size(); ++i)
            {
                auto opened = renderer->openView({1000U + static_cast<std::uint32_t>(i), 9}, {{64, 64}, true});
                require(opened, "view request accepted before asynchronous Scene identity validation");
                rejected_views[i] = std::move(*opened);
            }
            const auto view_deadline = Clock::now() + std::chrono::seconds{10};
            while (std::any_of(rejected_views.begin(), rejected_views.end(),
                               [](const auto &view) { return view->status().state != rendering::EViewState::FAILED; }))
            {
                require(Clock::now() < view_deadline && renderer->poll(64), "finite actual view error replies");
                std::this_thread::yield();
            }
            for (std::size_t i = 0; i < rejected_views.size(); ++i)
            {
                const auto expected =
                    lux::render::renderError<lux::render::err::scene::NotFound>(1000U + static_cast<std::uint32_t>(i));
                const auto status = rejected_views[i]->status();
                const auto image = rejected_views[i]->acquireImage();
                require(status.failure && status.failure->render_error.type == expected.type &&
                            status.failure->render_error.args == expected.args &&
                            status.failure->view == rejected_views[i]->id(),
                        "failed View status owns its exact backend error and full View identity");
                require(!image && image.error().render_error.args == expected.args,
                        "failed image query does not misreport NOT_READY");
                const auto sameFailure = [&](const rendering::RendererFailure &failure) {
                    const auto &original = *status.failure;
                    return failure.code == original.code && failure.render_error.type == original.render_error.type &&
                           failure.render_error.args == original.render_error.args && failure.view == original.view &&
                           failure.request == original.request && failure.backend_status == original.backend_status;
                };
                const auto before_stats = renderer->statistics();
                for (const auto extent : std::array<rendering::PixelExtent, 4>{{{96, 80}, {0, 0}, {96, 80}, {64, 64}}})
                {
                    const auto resized = rejected_views[i]->requestExtent(extent);
                    const auto after = rejected_views[i]->status();
                    const auto queried = rejected_views[i]->acquireImage();
                    const bool preserved = !resized && sameFailure(resized.error()) &&
                                           after.state == rendering::EViewState::FAILED &&
                                           after.request_sequence == status.request_sequence &&
                                           after.requested_extent == status.requested_extent && after.failure &&
                                           sameFailure(*after.failure) && !queried && sameFailure(queried.error());
                    failed_view_contract &= preserved;
                    std::printf("G01 view=%zu extent=%u,%u state=%u->%u sequence=%llu->%llu "
                                "request=%llu resize_code=%d image_code=%d identity_preserved=%u\n",
                                i, extent.width, extent.height, unsigned(status.state), unsigned(after.state),
                                status.request_sequence, after.request_sequence, status.failure->request,
                                resized ? -1 : int(resized.error().code), queried ? -1 : int(queried.error().code),
                                unsigned(preserved));
                }
                rendering::CameraFrame camera;
                camera.desired.session = 1;
                const auto camera_result = rejected_views[i]->setCamera(camera);
                const bool camera_preserved = !camera_result && sameFailure(camera_result.error());
                failed_view_contract &= camera_preserved;
                const auto after_stats = renderer->statistics();
                failed_view_contract &= before_stats.views == after_stats.views &&
                                        before_stats.descriptors_created == after_stats.descriptors_created &&
                                        before_stats.render_events == after_stats.render_events;
                std::printf("G01 camera_preserved=%u views=%zu/%zu descriptors=%llu/%llu events=%llu/%llu\n",
                            unsigned(camera_preserved), before_stats.views, after_stats.views,
                            before_stats.descriptors_created, after_stats.descriptors_created,
                            before_stats.render_events, after_stats.render_events);
                require(rejected_views[i]->beginClose(), "failed view close retains owner");
            }
            require(renderer->statistics().render_events == 2 && renderer->statistics().dropped_events == 1,
                    "bounded diagnostics overflow does not erase per-operation failure");
            auto diagnostic = renderer->takeDiagnostic();
            require(diagnostic && *diagnostic && (**diagnostic).failure.view == rejected_views[0]->id(),
                    "first owning diagnostic survives queue overflow");
            require(renderer->takeDiagnostic()->has_value() == false, "only retained diagnostic is delivered");
            for (auto &view : rejected_views)
            {
                for (;;)
                {
                    require(Clock::now() < view_deadline && renderer->poll(64), "failed view cleanup progresses");
                    auto closed = view->advanceClose();
                    require(closed, "failed View close step");
                    if (*closed == rendering::ERenderClose::COMPLETE)
                        break;
                }
                view.reset();
            }
            std::puts(
                "Actual view failures retained; diagnostic capacity=1, failures=2, dropped=1, both owners closed");
        }
        {
            FailingPane pane(*window);
            auto registration = window->uiSession().registerPane(pane);
            require(registration, "foreign pane registration");
            const lux::ui::FrameInfo frame{{1600, 900}, 1.0F / 60.0F, {1, 1}};
            require(window->beginFrame(frame), "foreign failure frame begins");
            const auto failed = window->drawPanes();
            require(!failed && failed.error().code == ui::EWindowError::ALLOCATION_FAILURE,
                    "foreign preparation error is contained");
            require(!window->frameOpen() && pane.calls == 1, "failed frame is discarded with scopes balanced");
            pane.fail = false;
            require(window->beginFrame(frame) && window->drawPanes(), "same window and pane retry after failure");
            auto snapshot = window->finishFrame();
            require(snapshot && snapshot->valid() && pane.calls == 2, "retry produces a valid owning snapshot");
            registration->reset();
        }
        auto metadata = examples::buildDevelopmentSceneMeta();
        require(metadata, "actual Scene metadata");
        auto shared_meta = std::make_shared<lux::scene::SceneMetaManager>(std::move(*metadata));
        auto source = coordinate_test
                          ? examples::openCoordinateScene({1}, messages.dispatcherRef(), *renderer, (*endpoint)->port(),
                                                          shared_meta, page_size)
                          : (variant == "alternate" ? examples::openAlternateScene : examples::openDevelopmentScene)(
                                {1}, messages.dispatcherRef(), *renderer, (*endpoint)->port(), shared_meta);
        require(source, "Scene source factory");
        if (variant == "resolved_source")
        {
            auto &registry = source->scene->registry();
            const auto entity = *registry.view<lux::simulation::ecs::Mesh3D>().begin();
            const auto visual = registry.get<lux::simulation::ecs::Mesh3D>(entity).value;
            const lux::scene::ResolvedMeshResources conflict{visual.material, visual.material, {}, {}};
            registry.emplace<lux::scene::ResolvedMeshResources>(entity, conflict);
            auto *const original_scene = source->scene.get();
            const auto original_selection = source->initial_selection;
            const auto before = renderer->statistics();
            auto attempted = sessions::SceneSession::openInspection(*source);
            const bool retained =
                source->scene.get() == original_scene && source->initial_selection == original_selection;
            resolved_source_contract = !attempted && retained &&
                                       attempted.error().code == sessions::ESceneError::STALE_CONTENT &&
                                       attempted.error().session == source->id;
            std::printf("R01 conflicting resolved source admitted=%d input_retained=%d error=%d session=%llu "
                        "leases=%zu->%zu views=%zu->%zu\n",
                        bool(attempted), retained, attempted ? -1 : int(attempted.error().code), source->id.value,
                        before.runtime_leases, renderer->statistics().runtime_leases, before.views,
                        renderer->statistics().views);
            if (attempted)
            {
                require((*attempted)->beginClose(), "R01 close previously admitted conflicting source");
                const auto deadline = Clock::now() + std::chrono::seconds{10};
                for (;;)
                {
                    const auto closed = (*attempted)->advanceClose();
                    require(closed, "R01 preserve explicit close result");
                    if (*closed == sessions::ECloseProgress::COMPLETE)
                        break;
                    require(Clock::now() < deadline && execution->drainMain(64) && renderer->poll(64),
                            "R01 conflicting source owner cleanup");
                }
                attempted->reset();
                source = examples::openDevelopmentScene({2}, messages.dispatcherRef(), *renderer, (*endpoint)->port(),
                                                        shared_meta);
                require(source, "R01 new independent valid source after closing rejected candidate");
            }
            else
            {
                require(retained && registry.get<lux::scene::ResolvedMeshResources>(entity) == conflict,
                        "R01 failure leaves Scene content and caller ownership intact");
                registry.remove<lux::scene::ResolvedMeshResources>(entity);
            }
        }
        if (coordinate_test)
        {
            auto &registry = source->scene->registry();
            for (const auto entity : registry.view<lux::simulation::ecs::Transform3D>())
                if (!registry.all_of<lux::simulation::ecs::Parent>(entity))
                    registry.patch<lux::simulation::ecs::Transform3D>(
                        entity, [&](auto &value) { value.translation += coordinate_offset; });
        }
        if (resource_publication_test)
        {
            std::size_t count{};
            for (const auto entity : source->scene->registry().view<lux::simulation::ecs::Mesh3D>())
            {
                if (!count && !resource_ready_test)
                    provider->failed_asset =
                        source->scene->registry().get<lux::simulation::ecs::Mesh3D>(entity).value.mesh;
                if (++count > 2)
                    source->scene->registry().remove<lux::simulation::ecs::Mesh3D>(entity);
            }
        }
        std::optional<sessions::SceneEntityRef> original_entity, recycled_entity;
        lux::asset::AssetId replacement_mesh;
        std::optional<sessions::ResourceRequestKey> superseded_key;
        if (gated_test)
            for (const auto entity : source->scene->registry().view<lux::simulation::ecs::Mesh3D>())
            {
                provider->held = source->scene->registry().get<lux::simulation::ecs::Mesh3D>(entity).value.mesh;
                if (partial_failure_test && !provider->contains(provider->held))
                    continue;
                original_entity = sessions::SceneEntityRef{source->id, entity};
                source->initial_selection = entity;
                break;
            }
        require(!gated_test || original_entity.has_value(), "delayed request matches an actual source mesh");
        if (late_source_test)
        {
            for (const auto entity : source->scene->registry().view<lux::simulation::ecs::Mesh3D>())
            {
                const auto mesh = source->scene->registry().get<lux::simulation::ecs::Mesh3D>(entity).value.mesh;
                if (mesh != provider->held)
                {
                    replacement_mesh = mesh;
                    break;
                }
            }
            require(!replacement_mesh.isNull(), "second real mesh source exists");
        }
        // Diagnostic variants are prepared before transferring the source into its authoritative Session.
        if (variant == "dim")
            for (const auto entity : source->scene->registry().view<lux::simulation::ecs::Light3D>())
                source->scene->registry().patch<lux::simulation::ecs::Light3D>(
                    entity, [](auto &value) { value.value.intensity *= 0.15F; });
        if (variant == "no_mesh")
            source->scene->registry().clear<lux::simulation::ecs::Mesh3D>();
        if (variant == "empty")
        {
            source->scene->registry().clear();
            source->initial_selection.reset();
        }
        if (churn_test)
            source->resource_capacity = 6;
        auto session_result = sessions::SceneSession::openInspection(*source);
        require(session_result && !source->scene, "Scene source transfer");
        auto session = std::move(*session_result);
#if defined(LUX_EDITOR_DIAGNOSTICS)
        if (resource_backpressure_test)
        {
            const sessions::SceneOwnerUpdate first{++prepared_cycles, 0};
            require(session->updateAtOwnerSafePoint(first), "R09 start real asset reads");
            require(session->advanceScene(first), "R09 finish initial Scene cycle before pausing consumer");
            require(window->beginFrame({{1600, 900}, 1.0F / 60, {1, 1}}) && window->drawPanes(),
                    "R09 prepare an actual UI frame to finish the current backend tick");
            auto wake_snapshot = window->finishFrame();
            require(wake_snapshot, "R09 capture real UI wake frame");
            auto wake_packet = renderer->sealFrame(*wake_snapshot, {});
            require(wake_packet, "R09 seal real UI wake frame");
            require(rendering::detail::RendererTestAccess::pauseConsumer(*renderer, true), "R09 request pause");
            const auto deadline = Clock::now() + std::chrono::seconds{15};
            while (!rendering::detail::RendererTestAccess::consumerPaused(*renderer))
            {
                if (wake_packet->valid())
                    require(renderer->trySubmitFrame(*wake_packet), "R09 admit actual wake packet");
                require(Clock::now() < deadline && renderer->poll(64), "R09 consumer reaches actual tick boundary");
                std::this_thread::yield();
            }
            while (!sessions::detail::SceneTestAccess::resourceReadsSettled(*session))
            {
                require(Clock::now() < deadline && execution->drainMain(64), "R09 actual provider replies complete");
                std::this_thread::yield();
            }
            static_cast<void>(sessions::detail::SceneTestAccess::resourceBackpressure(true));
            const sessions::SceneOwnerUpdate full{++prepared_cycles, 0};
            require(session->updateAtOwnerSafePoint(full), "R09 full queues retain retryable resource preparation");
            auto snapshot = session->readResources();
            require(snapshot && (*snapshot)->rows.size() == 3, "R09 all authoritative resource identities retained");
            backpressured_resources = *snapshot;
            const auto pressure = sessions::detail::SceneTestAccess::resourceBackpressure();
            require(pressure.control > 0 && pressure.upload > 0 && !renderer->controlAvailable(),
                    "R09 real Control ring and Upload admission both exhaust capacity");
            for (const auto &row : backpressured_resources->rows)
                require(row.state == sessions::ESceneResourceState::UPLOADING && !row.upload_failure &&
                            !row.asset_failure && row.render_failure.ok(),
                        "R09 backpressure is not resource failure");
            std::printf("R09 actual pressure control=%zu upload=%zu capacity=2 rows=%zu consumer_paused=1\n",
                        pressure.control, pressure.upload, backpressured_resources->rows.size());
            require(session->advanceScene(full), "R09 finish the prepared Scene cycle");
            if (backpressure_close)
            {
                auto *const retained = session.get();
                require(session->beginClose(), "R09 close under actual resource queue pressure");
                for (unsigned step = 0; step < 8; ++step)
                {
                    const auto closed = session->advanceClose();
                    require(closed && *closed == sessions::ECloseProgress::PENDING && session.get() == retained,
                            "R09 unfinished close returns PENDING and preserves its owner");
                }
                std::puts("R09 closing while consumer paused: eight PENDING results; same Session owner retained");
            }
            require(rendering::detail::RendererTestAccess::pauseConsumer(*renderer, false), "R09 resume real consumer");
            if (backpressure_close)
            {
                const auto close_deadline = Clock::now() + std::chrono::seconds{15};
                for (;;)
                {
                    require(execution->drainMain(64) && renderer->poll(64), "R09 advance actual resource replies");
                    const auto closed = session->advanceClose();
                    require(closed, "R09 preserve original close failure if any");
                    if (*closed == sessions::ECloseProgress::COMPLETE)
                        break;
                    require(Clock::now() < close_deadline, "R09 actual backpressure close completes");
                }
                session.reset();
                require(renderer->statistics().runtime_leases == 0,
                        "R09 Scene runtime and resources release both leases");
                source = examples::openDevelopmentScene({2}, messages.dispatcherRef(), *renderer, (*endpoint)->port(),
                                                        shared_meta);
                require(source, "R09 independent Scene after completing pressure close");
                auto reopened = sessions::SceneSession::openInspection(*source);
                require(reopened, "R09 same Renderer admits subsequent real Scene");
                session = std::move(*reopened);
                prepared_cycles = 0;
                std::puts("R09 pressure close COMPLETE; resource lease released; independent Scene admitted");
            }
        }
        if (shader_subfailure_test)
            sessions::detail::SceneTestAccess::failShaderInfoAfterMeshUpload();
        if (resource_publication_test)
        {
            ResourceObserver observer(messages.dispatcherRef());
            observer.session = session.get();
            auto connection = session->observe<sessions::SceneSession::resourcesChanged, &ResourceObserver::changed,
                                               lux::object::EDelivery::DIRECT>(observer);
            require(connection, "G02 real direct resource observer");
            require(session->updateAtOwnerSafePoint({++prepared_cycles, 0}), "G02 publish initial READING snapshot");
            auto initial = session->readResources();
            require(
                initial && (*initial)->rows.size() == 2 &&
                    std::all_of((*initial)->rows.begin(), (*initial)->rows.end(),
                                [](const auto &row) { return row.state == sessions::ESceneResourceState::READING; }),
                "G02 real reads held before owner accepts any completion");
            const auto deadline = Clock::now() + std::chrono::seconds{10};
            if (resource_ready_test)
            {
                sessions::detail::SceneTestAccess::holdResourceAdoption(true);
                provider->secondary_held = (*initial)->rows.back().key.material;
                std::printf("G02 fixture A=%u/%u B=%u/%u\n",
                            std::to_integer<unsigned>((*initial)->rows.front().key.mesh.bytes().back()),
                            std::to_integer<unsigned>((*initial)->rows.front().key.material.bytes().back()),
                            std::to_integer<unsigned>((*initial)->rows.back().key.mesh.bytes().back()),
                            std::to_integer<unsigned>((*initial)->rows.back().key.material.bytes().back()));
                std::fflush(stdout);
            }
            provider->release();
            if (resource_ready_test)
            {
                rendering::EditorFramePacket progress_packet;
                while (!sessions::detail::SceneTestAccess::resourceReadyForAdoption(*session))
                {
                    require(Clock::now() < deadline, "G02 real material upload reply before owner adoption");
                    const sessions::SceneOwnerUpdate update{++prepared_cycles, 0};
                    require(session->updateAtOwnerSafePoint(update) && session->advanceScene(update) &&
                                renderer->poll(64),
                            "G02 real mesh/shader/material pipeline advances while sibling read is held");
                    if (!progress_packet.valid())
                    {
                        require(window->beginFrame({{1600, 900}, 0.016F, {1, 1}}) && window->drawPanes(),
                                "G02 actual frame advances upload device completion");
                        auto snapshot = window->finishFrame();
                        require(snapshot, "G02 owner frame snapshot");
                        auto packet = renderer->sealFrame(*snapshot, {});
                        require(packet && !snapshot->valid(), "G02 progress frame admission preparation");
                        progress_packet = std::move(*packet);
                    }
                    require(renderer->trySubmitFrame(progress_packet), "G02 retain any backpressured progress frame");
                    if (prepared_cycles % 1000 == 0)
                    {
                        const auto observation = sessions::detail::SceneTestAccess::resourceOwnerSnapshot(*session);
                        require(observation, "G02 waiting owner observation");
                        std::printf("G02 waiting states=%u,%u provider=%u/%u frames=%llu\n",
                                    unsigned((*observation)->rows[0].state), unsigned((*observation)->rows[1].state),
                                    provider->entered.load(), provider->returned.load(), renderer->statistics().frames);
                        std::fflush(stdout);
                    }
                    std::this_thread::yield();
                }
                initial = session->readResources();
                require(initial && (*initial)->rows.front().state == sessions::ESceneResourceState::UPLOADING &&
                            (*initial)->rows.back().state == sessions::ESceneResourceState::READING,
                        "G02 actual A upload completes before READY adoption; B read is still held");
                provider->secondary_released.store(true, std::memory_order_release);
                provider->secondary_released.notify_all();
            }
            while (!sessions::detail::SceneTestAccess::resourceReadsSettled(*session))
            {
                require(Clock::now() < deadline, "G02 actual asset reads complete");
                std::this_thread::yield();
            }
            if (resource_ready_test)
                sessions::detail::SceneTestAccess::holdResourceAdoption(false);
            if (resource_snapshot_test)
                lux_er1_scene_allocation_fail_after(0);
            else
                sessions::detail::SceneTestAccess::failNextShaderPreparation();
            const auto notices_before = observer.calls;
            const auto failed = session->updateAtOwnerSafePoint({++prepared_cycles, 0});
            const auto allocations =
                resource_snapshot_test ? lux_er1_scene_allocation_disarm() : lux_er1_client_allocation_disarm();
            require(!failed && failed.error().code == sessions::ESceneError::ALLOCATION_FAILURE &&
                        failed.error().session == session->id() && allocations == 1,
                    "G02 exact real client shader preparation allocation failure contained by Scene owner");
            require(observer.calls == notices_before, "G02 failed preparation cannot notify an unpublished snapshot");
            const auto actual = sessions::detail::SceneTestAccess::resourceOwnerSnapshot(*session);
            require(actual, "G02 observe retained owner without mutating it");
            require(session->updateAtOwnerSafePoint({prepared_cycles, 0}),
                    "G02 retry same owner cycle without backend reply pump");
            const auto published = session->readResources();
            require(published, "G02 read retry snapshot");
            bool found_failure{};
            for (std::size_t index = 0; index < (*actual)->rows.size(); ++index)
            {
                const auto &row = (*actual)->rows[index];
                const auto &visible = (*published)->rows[index];
                if (row.state == sessions::ESceneResourceState::FAILED)
                {
                    require(row.asset_failure && row.key.mesh == provider->failed_asset,
                            "G02 actual failed provider identity retained by resource owner");
                    found_failure = true;
                    resource_publication_contract &= visible.state == row.state && visible.key == row.key &&
                                                     visible.asset_failure &&
                                                     visible.asset_failure->code == row.asset_failure->code;
                }
                if (resource_ready_test && row.state == sessions::ESceneResourceState::READY)
                {
                    found_failure = true;
                    resource_publication_contract &= visible.state == row.state && visible.key == row.key;
                }
                std::printf("G02 row=%zu owner_state=%u published_state=%u request=%llu revision=%llu->%llu\n", index,
                            unsigned(row.state), unsigned(visible.state), row.key.sequence, (*initial)->revision,
                            (*published)->revision);
            }
            require(found_failure, "G02 required failure or READY transition occurred before preparation OOM");
            resource_publication_contract &= (*published)->revision > (*initial)->revision;
            resource_publication_contract &= observer.calls == notices_before + 1;
            const auto owner_after_retry = sessions::detail::SceneTestAccess::resourceOwnerSnapshot(*session);
            require(owner_after_retry && (*owner_after_retry)->rows.size() == (*actual)->rows.size(),
                    "G02 retry retains every original request owner");
            for (std::size_t index = 0; index < (*actual)->rows.size(); ++index)
                require((*owner_after_retry)->rows[index].key == (*actual)->rows[index].key &&
                            (*owner_after_retry)->rows[index].state == (*actual)->rows[index].state,
                        "G02 retry publishes pending changes without a new row transition");
            require(session->updateAtOwnerSafePoint({++prepared_cycles, 0}), "G02 static owner cycle");
            resource_publication_contract &=
                session->readResources()->get() == published->get() && observer.calls == notices_before + 1;
            std::printf("G02 allocation_attempts=%zu publication_preserved=%u views=%zu leases=%zu\n", allocations,
                        unsigned(resource_publication_contract), renderer->statistics().views,
                        renderer->statistics().runtime_leases);
            std::fflush(stdout);
        }
#endif
        std::unique_ptr<sessions::SceneView> separate_view;
        rendering::PixelExtent separate_extent =
            multiple_equal ? rendering::PixelExtent{1014, 593} : rendering::PixelExtent{256, 256};
        const auto open_second = [&] {
            auto opened = sessions::SceneView::create(messages.dispatcherRef(), *session, *renderer);
            require(opened, "independent second SceneView of the same Session");
            separate_view = std::move(*opened);
            require(separate_view->requestExtent(separate_extent), "second view has its own extent");
        };
        if (multiple_reverse)
            open_second();
        auto view_result = sessions::SceneView::create(messages.dispatcherRef(), *session, *renderer);
        require(view_result, "Scene view factory");
        auto view = std::move(*view_result);
        if (multiple_views_test)
            require(view->requestExtent({1014, 593}), "primary extent admitted before startup progress");
        auto *camera = view.get(); // Narrow borrowed SceneView; workspace owns it until explicit close completes.
        if (coordinate_test)
        {
            const auto basis = sessions::SceneCamera{}.view(Eigen::Vector3d::Zero());
            sessions::CameraMotion motion;
            motion.pan_delta = {basis.block<1, 3>(0, 0).dot(coordinate_offset),
                                basis.block<1, 3>(1, 0).dot(coordinate_offset)};
            motion.dolly = -basis.block<1, 3>(2, 0).dot(coordinate_offset);
            require(camera->moveCamera(motion), "G03 move real camera with the translated scene");
        }
        auto workspace_result = ui::SceneWorkspace::create(*window, {1}, *session, view);
        require(workspace_result && !view, "workspace view transfer");
        auto workspace = std::move(*workspace_result);
        require(workspace->activate(), "workspace activation");
        foreignOwnerCalls(*window, *renderer, *session, *camera, *workspace);
#if defined(LUX_EDITOR_DIAGNOSTICS)
        if (workspace_failure_test)
        {
            const auto closeProbe = [&](std::unique_ptr<ui::SceneWorkspace> &probe) {
                require(probe->beginClose(), "G04 probe retains close owner");
                const auto deadline = Clock::now() + std::chrono::seconds{10};
                for (;;)
                {
                    require(Clock::now() < deadline && renderer->poll(64), "G04 close progresses");
                    const auto result = probe->advanceClose();
                    require(result, "G04 real close completion");
                    if (*result == sessions::ECloseProgress::COMPLETE)
                        break;
                }
                probe.reset();
            };
            const auto before = renderer->statistics();
            rendering::detail::RendererTestAccess::useSceneForNextView({1001, 9});
            auto rejected_view = sessions::SceneView::create(messages.dispatcherRef(), *session, *renderer);
            require(rejected_view, "G04 SceneView owner exists before backend validates source Scene");
            auto *borrowed = rejected_view->get();
            auto probe = ui::SceneWorkspace::create(*window, {7}, *session, *rejected_view);
            require(probe && !*rejected_view && borrowed->requestExtent({64, 64}), "G04 owned Workspace probe");
            const auto deadline = Clock::now() + std::chrono::seconds{10};
            std::optional<sessions::SceneFailure> original;
            while (!original)
            {
                require(Clock::now() < deadline && renderer->poll(64), "G04 real invalid Scene reply arrives");
                const auto result = borrowed->synchronize();
                if (!result)
                    original = result.error();
            }
            const auto expected = lux::render::renderError<lux::render::err::scene::NotFound>(1001);
            require(original->renderer && original->renderer->render_error.type == expected.type &&
                        original->renderer->render_error.args == expected.args,
                    "G04 backend failure is exact scene::NotFound");
            const auto propagated = (*probe)->updateBeforeFrame();
            const bool sync_preserved = !propagated && preservesSceneFailure(propagated.error(), *original);
            workspace_failure_contract &= sync_preserved;
            std::printf("G04 sync scene_code=%u renderer_code=%u request=%llu session=%llu preserved=%u\n",
                        unsigned(original->code), unsigned(original->renderer->code), original->renderer->request,
                        original->session.value, unsigned(sync_preserved));
            closeProbe(*probe);
            auto closing_view = sessions::SceneView::create(messages.dispatcherRef(), *session, *renderer);
            require(closing_view, "G04 close observer actual SceneView");
            auto *closing_borrow = closing_view->get();
            auto closing_workspace = ui::SceneWorkspace::create(*window, {8}, *session, *closing_view);
            require(closing_workspace, "G04 close observer actual Workspace");
            ViewCloseObserver observer(messages.dispatcherRef());
            observer.view = closing_borrow;
            observer.workspace = closing_workspace->get();
            auto connection = closing_borrow->observe<sessions::SceneView::imageChanged, &ViewCloseObserver::changed,
                                                      lux::object::EDelivery::DIRECT>(observer);
            require(connection && renderer->poll(64) && closing_borrow->synchronize(), "G04 direct View notice");
            require(observer.calls == 1, "G04 close failure callback actually ran once");
            workspace_failure_contract &= observer.preserved;
            closeProbe(*closing_workspace);
            const auto after = renderer->statistics();
            require(after.views == before.views && after.runtime_leases == before.runtime_leases,
                    "G04 failures retain then release exactly their own View/Session association");
            std::printf("G04 cleanup views=%zu/%zu leases=%zu/%zu events=%llu/%llu\n", before.views, after.views,
                        before.runtime_leases, after.runtime_leases, before.render_events, after.render_events);
            std::fflush(stdout);
        }
#endif
        {
            auto closing_view = sessions::SceneView::create(messages.dispatcherRef(), *session, *renderer);
            require(closing_view, "viewport failure probe owns its SceneView");
            ui::SceneViewport viewport(messages.dispatcherRef(), lux::ui::PaneId{"er1.failure.viewport"},
                                       **closing_view);
            require((*closing_view)->beginClose(), "begin closing viewport failure probe");
            viewport.consumeInput({}, 0.016, {1, 1});
            const auto failure = viewport.actionFailure();
            require(failure && failure->session == (*closing_view)->sessionId() && failure->renderer &&
                        failure->renderer->code == rendering::ERendererError::STOPPING,
                    "viewport retains exact failed extent action and its Session/render error");
            std::thread foreign_pane([&] {
                viewport.consumeInput({}, 0.016, {1, 1});
                viewport.cancelCapture();
                viewport.releaseFrameImages();
                require(viewport.frameImages().empty() && viewport.actionFailure() &&
                            viewport.actionFailure()->code == sessions::ESceneError::WRONG_THREAD,
                        "foreign Pane calls cannot borrow local state or overwrite its action error");
            });
            foreign_pane.join();
            require(viewport.actionFailure()->renderer->code == rendering::ERendererError::STOPPING &&
                        viewport.actionFailure()->session == failure->session,
                    "foreign consumeInput leaves owning-thread failure unchanged");
            const auto closing_deadline = Clock::now() + std::chrono::seconds{10};
            for (;;)
            {
                require(Clock::now() < closing_deadline, "viewport failure probe finite close");
                require(renderer->poll(64), "viewport failure probe renderer progress");
                const auto closed = (*closing_view)->advanceClose();
                require(closed, "viewport failure probe retains owner until close completes");
                if (*closed == sessions::ECloseProgress::COMPLETE)
                    break;
                std::this_thread::yield();
            }
        }
        {
            const auto shared_history = session->historyId();
            auto second_view = sessions::SceneView::create(messages.dispatcherRef(), *session, *renderer);
            require(second_view, "second view of the same authoritative Session");
            {
                FailingPane conflict(*window, "lux.scene.workspace.2.toolbar");
                auto conflict_registration = window->uiSession().registerPane(conflict);
                require(conflict_registration, "reserve final pane identity to force factory rollback");
                auto rejected = ui::SceneWorkspace::create(*window, {2}, *session, *second_view);
                require(!rejected && *second_view, "final pane registration failure retains caller view");
                conflict_registration->reset();
            }
            auto second_workspace = ui::SceneWorkspace::create(*window, {2}, *session, *second_view);
            require(second_workspace && !*second_view, "second workspace retains shared window history registration");
            require((*second_workspace)->beginClose(), "close only the second workspace");
            const auto partial_close_deadline = Clock::now() + std::chrono::seconds{10};
            for (;;)
            {
                require(Clock::now() < partial_close_deadline, "second workspace finite close");
                require(renderer->poll(64), "second view close progress");
                const auto closed = (*second_workspace)->advanceClose();
                require(closed, "second workspace close step");
                if (*closed == sessions::ECloseProgress::COMPLETE)
                    break;
                std::this_thread::yield();
            }
            second_workspace->reset();
            require(session->state() == sessions::ESessionState::READY && session->historyId() == shared_history,
                    "closing one workspace preserves Session and shared history");
            require(window->activeHistory().view()->has_target && workspace->activate(),
                    "remaining workspace keeps its history registration and can activate");
        }
        auto runtime_result = renderer->acquire();
        require(runtime_result, "diagnostic render lease");
        auto runtime = std::move(*runtime_result);
        if (multiple_views_test && !separate_view)
            open_second();
        bool separate_closing{}, separate_closed{};
        rendering::EditorFramePacket pending;
        rendering::ViewImage retained;
        rendering::ViewImage evidence_image;
        Readback readback;
        Readback second_readback;
        std::array<std::uint64_t, 4> second_checksums{};
        std::vector<std::uint64_t> checksums(churn_test ? 12 : (dynamic_test ? 5 : (multiple_lifecycle ? 4 : 2)));
        std::shared_ptr<const sessions::SceneResourceSnapshot> initial_resources;
        std::optional<sessions::detail::ESceneTestMutation> pending_mutation;
        unsigned phase{};
        const auto needs_second = [&] { return multiple_views_test && (!multiple_lifecycle || phase < 3); };
        std::uint64_t next_capture = 60, cycle = prepared_cycles;
        bool recovery_mounted{}, retry_accepted{};
        bool partial_failure_observed{};
        unsigned held_selection_steps{};
        std::optional<sessions::ResourceRequestKey> selection_request;
        bool frame_protocol_checked{};
        std::uint64_t failed_packet_sequence{};
        const auto injected_error = lux::render::renderError<lux::render::err::memory::GpuAllocationFailed>();
        std::optional<sessions::ResourceRequestKey> failed_key;
        std::shared_ptr<const sessions::SceneResourceSnapshot> failed_snapshot;
        const auto deadline = Clock::now() + std::chrono::seconds{45};
        while (phase < checksums.size() || (record_failure_test && !failed_packet_sequence))
        {
            require(Clock::now() < deadline, "finite GPU run deadline");
            require(window->collectInput(), "input collection");
            require(execution->drainMain(64), "main continuations");
            require(renderer->poll(64), "renderer poll");
            if (separate_closing && separate_view)
            {
                auto closed = separate_view->advanceClose();
                require(closed, "second view close remains explicit while first keeps rendering");
                if (*closed == sessions::ECloseProgress::COMPLETE)
                {
                    separate_view.reset();
                    separate_closed = true;
                }
            }
            if (pending.valid())
                require(renderer->trySubmitFrame(pending), "retry retained frame");
#if defined(LUX_EDITOR_DIAGNOSTICS)
            if (pending_mutation)
            {
                require(sessions::detail::SceneTestAccess::mutateSource(*session, *pending_mutation),
                        "diagnostic source change between authoritative owner cycles");
                pending_mutation.reset();
            }
            if (late_entity_test && !recycled_entity && provider->entered.load() && renderer->statistics().frames >= 30)
            {
                auto recycled = sessions::detail::SceneTestAccess::recycleSelectedEntity(*session);
                require(recycled && recycled->entity != original_entity->entity,
                        "real entity generation changes while its mesh read is in flight");
                recycled_entity = *recycled;
                provider->release();
            }
#endif
            const sessions::SceneOwnerUpdate update{++cycle, 1.0 / 60.0};
#if defined(LUX_EDITOR_DIAGNOSTICS)
            if (late_source_test && !superseded_key && provider->entered.load() && renderer->statistics().frames >= 30)
            {
                const auto before = session->readResources();
                require(before, "owning source request snapshot");
                for (const auto &row : (*before)->rows)
                    if (row.key.target == *original_entity)
                        superseded_key = row.key;
                require(superseded_key.has_value(), "original in-flight source request identity");
                require(
                    sessions::detail::SceneTestAccess::replaceMeshSource(*session, *original_entity, replacement_mesh),
                    "replace actual Mesh3D source while old bytes remain in flight");
                provider->release();
            }
#endif
            require(session->updateAtOwnerSafePoint(update), "Scene owner update");
            if (late_source_test && superseded_key && session->selection().current)
                require(session->select(std::nullopt), "selection is independent of changed source adoption");
            require(workspace->updateBeforeFrame(), "view synchronization");
            auto resources = session->readResources();
            require(resources, "resource status snapshot");
            if (late_selection_test && provider->entered.load() && !provider->released.load())
            {
                const auto waiting = std::find_if((*resources)->rows.begin(), (*resources)->rows.end(),
                                                  [&](const auto &row) { return row.key.target == *original_entity; });
                require(waiting != (*resources)->rows.end() && waiting->state == sessions::ESceneResourceState::READING,
                        "held asset stays READING while selection changes");
                if (!selection_request)
                {
                    selection_request = waiting->key;
                    const auto outline = session->readOutline();
                    require(outline, "selection test uses the actual Session outline");
                    const auto other = std::find_if((*outline)->rows.begin(), (*outline)->rows.end(),
                                                    [&](const auto &row) { return row.target != *original_entity; });
                    require(other != (*outline)->rows.end() && session->select(other->target),
                            "select B while A asset read is in flight");
                    require(session->selection().current == other->target, "selection B was actually committed");
                }
                require(waiting->key == *selection_request && provider->returned.load() == 0,
                        "selection preserves the owning request and does not complete the held provider");
                ++held_selection_steps;
                if (held_selection_steps == 16)
                    require(session->select(std::nullopt), "clear selection while A remains in flight");
                if (held_selection_steps == 32)
                {
                    require(!session->selection().current, "selection remains empty before release");
                    provider->release();
                }
            }
            if (partial_failure_test)
            {
                for (const auto &row : (*resources)->rows)
                    if (row.state == sessions::ESceneResourceState::FAILED)
                    {
                        require(row.asset_failure.has_value(), "partial sibling failure owns its asset error");
                        partial_failure_observed = true;
                    }
            }
            if (retry_test && !retry_accepted)
            {
                const auto failed =
                    std::find_if((*resources)->rows.begin(), (*resources)->rows.end(),
                                 [](const auto &row) { return row.state == sessions::ESceneResourceState::FAILED; });
                if (failed != (*resources)->rows.end())
                {
                    require(failed->asset_failure.has_value(), "original asset failure survives resource polling");
                    require(failed->asset_failure->code ==
                                lux::process::asset_loading::EAssetLoadError::STORAGE_FAILURE,
                            "real missing asset is a storage failure");
                    if (!recovery_mounted)
                    {
                        failed_snapshot = *resources;
                        failed_key = failed->key;
                        auto recovery = lux::asset::PakAssetProvider::loadFromFile(argv[4]);
                        require(recovery, "load recovery package");
                        require(vfs.mount({"/Recovery", *recovery, 100}) != lux::asset::kInvalidMountId,
                                "publish complete resource provider");
                        require(session->select(std::nullopt), "clear selection independently from resource retry");
                        recovery_mounted = true;
                    }
                    auto retried = session->retryResources(*failed_key);
                    require(retried || retried.error().code == sessions::ESceneError::BUSY,
                            "retry either retains previous work or admits replacement");
                    if (retried)
                    {
                        retry_accepted = true;
                        const auto stale = session->retryResources(*failed_key);
                        require(!stale && stale.error().code == sessions::ESceneError::STALE_CONTENT,
                                "old retry token is obsolete after sequence replacement");
                    }
                }
            }
            const auto ready_count =
                std::count_if((*resources)->rows.begin(), (*resources)->rows.end(),
                              [](const auto &row) { return row.state == sessions::ESceneResourceState::READY; });
            const bool ready =
                (churn_test && ready_count == 3) ||
                (!churn_test && std::all_of((*resources)->rows.begin(), (*resources)->rows.end(), [&](const auto &row) {
                    return row.state == sessions::ESceneResourceState::READY ||
                           (shader_subfailure_test && row.backend_status == 1 &&
                            row.state == sessions::ESceneResourceState::FAILED) ||
                           (resource_publication_test && row.asset_failure &&
                            row.state == sessions::ESceneResourceState::FAILED) ||
                           (dynamic_test && phase >= 3 && row.state == sessions::ESceneResourceState::SUPERSEDED);
                }));
            if (churn_test)
                require((*resources)->rows.size() <= 6, "bounded resource ledger while replacing identities");
            else if (has_mesh && !(dynamic_test && phase >= 3) && !late_entity_test && !late_source_test)
                require((*resources)->rows.size() == (resource_publication_test ? 2 : 3),
                        "discover every visual entity in the actual input");
            if (readback.request.valid() && readback.request.isReady() &&
                (!needs_second() || second_readback.request.isReady()))
            {
                if (needs_second())
                    second_checksums[phase] = second_readback.finish(
                        output / (std::string(variant) + "-second-" + std::to_string(phase) + ".ppm"));
                checksums[phase] =
                    readback.finish(output / (std::string(variant) + "-" + std::to_string(phase) + ".ppm"));
                ++phase;
                if (multiple_lifecycle && phase == 2)
                {
                    separate_extent = {384, 192};
                    require(separate_view->requestExtent(separate_extent), "resize only second view");
                }
                if (multiple_lifecycle && phase == 3)
                {
                    require(separate_view->beginClose(), "close second view while first continues");
                    separate_closing = true;
                }
                if (churn_test && phase < checksums.size())
                    pending_mutation = sessions::detail::ESceneTestMutation::ROTATE_MESH_SOURCES;
                else if (dynamic_test && phase == 1)
                    pending_mutation = sessions::detail::ESceneTestMutation::ADJUST_VISUALS;
                else if (dynamic_test && phase == 3)
                    pending_mutation = sessions::detail::ESceneTestMutation::REMOVE_VISUALS;
                else if (dynamic_test && phase == 4)
                    pending_mutation = sessions::detail::ESceneTestMutation::CLEAR_SCENE;
                else if ((!dynamic_test && phase == 1) || (dynamic_test && phase == 2))
                {
                    const auto history = session->historyView();
                    sessions::CameraMotion motion;
                    motion.angular_delta = {0.35, 0.1};
                    require(camera->moveCamera(motion), "camera motion");
                    require(session->historyView()->history.current == history->history.current,
                            "camera never dirties content history");
                }
                next_capture = renderer->statistics().frames + 45;
            }
            std::uint32_t width{}, height{};
            window->nativeWindow().size(width, height);
            require(window->beginFrame({{float(width), float(height)}, 1.0F / 60.0F, {1, 1}}), "UI begin");
            require(window->drawPanes(), "draw real Scene panes");
            require(workspace->afterDraw(1.0 / 60.0, {1, 1}), "concrete pane input");
            std::vector<rendering::ViewImage> frame_images(workspace->frameImages().begin(),
                                                           workspace->frameImages().end());
            if (separate_view && !separate_closing)
            {
                require(separate_view->synchronize(), "second view synchronizes independently");
                auto second_image = separate_view->image();
                if (second_image)
                    frame_images.push_back(std::move(*second_image));
                else
                    require(second_image.error().code == sessions::ESceneError::NOT_READY,
                            "second view either produces its image or is still preparing");
            }
            const std::span<const rendering::ViewImage> images{frame_images};
            if (renderer->statistics().frames >= 70)
                require(!workspace->frameImages().empty(), "stable viewport retains an image on every UI frame");
            if (ready && !images.empty() && phase < checksums.size() && !readback.request.valid() &&
                renderer->statistics().frames >= next_capture && renderer->controlAvailable(needs_second() ? 2 : 1) &&
                (!needs_second() || images.size() == 2) && (!multiple_lifecycle || phase != 3 || separate_closed))
            {
                retained = images.front();
                if (churn_test && !initial_resources)
                    initial_resources = *resources;
                readback.start(runtime, images.front());
                if (needs_second())
                {
                    require(images[0].view != images[1].view &&
                                images[0].content.source.session == images[1].content.source.session,
                            "two actual images share only the content Session");
                    require(images[1].extent == separate_extent, "independent second view extent");
                    if (multiple_equal)
                        require(images[0].extent == images[1].extent, "equal-extent physical targets remain distinct");
                    const auto first_target = rendering::detail::ViewImageAccess::record(images[0])->version->target;
                    const auto second_target = rendering::detail::ViewImageAccess::record(images[1])->version->target;
                    require(first_target != second_target, "two real targets");
                    if (phase == 0)
                    {
                        std::printf("view targets first=%u:%u second=%u:%u reverse=%u\n", first_target.index,
                                    first_target.gen, second_target.index, second_target.gen, multiple_reverse);
                        std::fflush(stdout);
                        require(multiple_reverse ? second_target.index < first_target.index
                                                 : first_target.index < second_target.index,
                                "actual target allocation/record order matches requested variant");
                    }
                    second_readback.start(runtime, images[1]);
                }
            }
            require(session->advanceScene(update), "Scene simulation and presentation");
            auto snapshot = window->finishFrame();
            require(snapshot, "owning UI snapshot");
            if (!frame_protocol_checked && !workspace->frameImages().empty() && !session->presentationPending())
            {
                const auto original_images = workspace->frameImages();
                const auto original_texture = original_images.front().texture;
                std::thread foreign_images([&] {
                    require(workspace->frameImages().empty(), "foreign Workspace cannot borrow live frame images");
                    workspace->releaseFrameImages();
                });
                foreign_images.join();
                require(workspace->frameImages().data() == original_images.data() &&
                            workspace->frameImages().size() == original_images.size() &&
                            workspace->frameImages().front().texture == original_texture,
                        "foreign release preserves actual current Pane image owner and texture");
                const auto *camera_record =
                    rendering::detail::ViewImageAccess::record(workspace->frameImages().front());
                require(coordinate_test || camera_record->camera.origin == std::array<double, 3>{6, 4, 8},
                        "real wire origin carries the camera position, not its enclosing page origin");
                if (coordinate_test)
                {
                    const auto &wire = camera_record->wire_camera;
                    coordinate_contract &= wire.coordinate_page_size == page_size;
                    for (std::size_t axis = 0; axis < 3; ++axis)
                    {
                        const auto decoded =
                            wire.render_origin.page_delta[axis] * page_size + wire.render_origin.local[axis];
                        coordinate_contract &= std::abs(decoded - camera_record->camera.origin[axis]) < 0.001;
                    }
                    std::printf("G03 scene_page=%.0f wire_page=%.0f page=%d,%d,%d local=%.3f,%.3f,%.3f valid=%u\n",
                                page_size, double(wire.coordinate_page_size), wire.render_origin.page_delta[0],
                                wire.render_origin.page_delta[1], wire.render_origin.page_delta[2],
                                double(wire.render_origin.local[0]), double(wire.render_origin.local[1]),
                                double(wire.render_origin.local[2]), unsigned(coordinate_contract));
                    std::fflush(stdout);
                }
                require(camera_record->wire_camera.view_matrix[12] == 0 &&
                            camera_record->wire_camera.view_matrix[13] == 0 &&
                            camera_record->wire_camera.view_matrix[14] == 0,
                        "rotation-only view matches the existing CPU/GPU camera protocol");
#if defined(LUX_EDITOR_DIAGNOSTICS)
                if (variant == "factory_failure")
                {
                    const auto accepted_before = renderer->statistics().accepted_frames;
                    const std::vector tokens(snapshot->textures().begin(), snapshot->textures().end());
                    std::vector<long> image_references;
                    for (const auto &image : images)
                        image_references.push_back(rendering::detail::ViewImageAccess::references(image));
                    std::size_t failures{};
                    for (std::size_t index = 0; index < 128; ++index)
                    {
                        std::fprintf(stderr, "seal allocation index=%zu\n", index);
                        lux_er1_renderer_allocation_fail_after(index);
                        auto attempted = renderer->sealFrame(*snapshot, images);
                        const auto allocations = lux_er1_renderer_allocation_disarm();
                        if (attempted)
                        {
                            require(allocations == index && !snapshot->valid(), "seal allocation sweep reached commit");
                            break;
                        }
                        ++failures;
                        require(allocations == index + 1 &&
                                    attempted.error().code == rendering::ERendererError::ALLOCATION_FAILURE,
                                "actual DLL seal allocation failed");
                        require(snapshot->valid() && std::ranges::equal(snapshot->textures(), tokens) &&
                                    renderer->statistics().accepted_frames == accepted_before,
                                "failed seal retains original snapshot and performs no admission");
                        for (std::size_t i = 0; i < images.size(); ++i)
                            require(rendering::detail::ViewImageAccess::references(images[i]) == image_references[i],
                                    "each failed seal returns every temporarily acquired image reference");
                    }
                    require(failures > 2 && failures < 128, "all actual seal allocation points retried");
                    std::printf("Frame seal actual DLL allocation failures checked: %zu\n", failures);
                    auto recaptured = window->uiSession().captureFrame();
                    require(recaptured, "recapture completed frame after allocation test without input replay");
                    *snapshot = std::move(*recaptured);
                }
#endif
                auto forged = images.front();
                ++forged.view.generation;
                auto rejected = renderer->sealFrame(*snapshot, {&forged, 1});
                require(!rejected && rejected.error().code == rendering::ERendererError::STALE_IMAGE &&
                            snapshot->valid(),
                        "forged view generation rejected without consuming snapshot");
                forged = images.front();
                ++forged.extent.width;
                rejected = renderer->sealFrame(*snapshot, {&forged, 1});
                require(!rejected && snapshot->valid(), "forged extent rejected without consuming snapshot");
                auto second_record = camera->image();
                require(second_record, "second independent image record for the same texture");
                const std::array duplicates{images.front(), *second_record};
                rejected = renderer->sealFrame(*snapshot, duplicates);
                require(!rejected && rejected.error().code == rendering::ERendererError::STALE_IMAGE &&
                            snapshot->valid(),
                        "same texture with different immutable records rejected");
                rendering::EditorFramePacket blocked;
                for (std::size_t i = 0; i <= config.frame_capacity; ++i)
                {
                    auto copy = window->uiSession().captureFrame();
                    require(copy, "capture finished frame again without drawing panes or replaying input");
                    auto sealed = renderer->sealFrame(*copy, images);
                    require(sealed && !copy->valid(), "independent owning frame seal");
                    const auto sequence = sealed->sequence();
                    const auto admitted = renderer->trySubmitFrame(*sealed);
                    require(admitted, "bounded frame admission");
                    if (*admitted == rendering::EFrameSubmit::BACKPRESSURED)
                    {
                        require(sealed->valid() && sealed->sequence() == sequence,
                                "backpressure retains exactly the original packet");
                        blocked = std::move(*sealed);
                        break;
                    }
                    require(!sealed->valid(), "accepted frame consumed exactly once");
                }
                require(blocked.valid(), "actually filled the renderer-owned queue");
                const auto original_sequence = blocked.sequence();
                const auto backpressure_deadline = Clock::now() + std::chrono::seconds{5};
                while (blocked.valid())
                {
                    require(Clock::now() < backpressure_deadline, "finite backpressure retry");
                    require(renderer->poll(64), "release actual queue capacity");
                    require(blocked.sequence() == original_sequence, "same blocked packet retried");
                    require(renderer->trySubmitFrame(blocked), "retry without UI execution");
                }
                auto copy = window->uiSession().captureFrame();
                require(copy, "move-overwrite source capture");
                auto first = renderer->sealFrame(*copy, images);
                copy = window->uiSession().captureFrame();
                require(copy, "move-overwrite destination capture");
                auto second = renderer->sealFrame(*copy, images);
                require(first && second, "two unsubmitted owning packets");
                const auto source_sequence = first->sequence();
                const auto accepted = renderer->statistics().accepted_frames;
                *second = std::move(*first);
                require(!first->valid() && second->sequence() == source_sequence &&
                            renderer->statistics().accepted_frames == accepted,
                        "packet move-overwrite only releases old ownership and transfers the source");
                frame_protocol_checked = true;
                evidence_image = images.front();
            }
            if (!session->presentationPending())
            {
                // Missing image references must fail without consuming the same input that we retry below.
                if (!snapshot->textures().empty())
                {
                    auto rejected = renderer->sealFrame(*snapshot, {});
                    require(!rejected && snapshot->valid(), "failed seal retains input");
                }
                auto packet = renderer->sealFrame(*snapshot, images);
                require(packet && !snapshot->valid(), "successful seal consumes input");
                pending = std::move(*packet);
#if defined(LUX_EDITOR_DIAGNOSTICS)
                if (record_failure_test && phase == checksums.size())
                {
                    failed_packet_sequence = pending.sequence();
                    require(rendering::detail::RendererTestAccess::failRecord(pending, injected_error),
                            "inject a typed record-preparation failure into a real owning packet");
                }
#endif
                auto submitted = renderer->trySubmitFrame(pending);
                require(submitted, "frame admission");
                require((*submitted == rendering::EFrameSubmit::SUBMITTED) != pending.valid(),
                        "frame ownership result");
            }
            workspace->releaseFrameImages();
            if (late_close_test && provider->entered.load() && renderer->statistics().frames >= 40 && !images.empty() &&
                (!partial_failure_test || partial_failure_observed))
            {
                // Preserve an actual CPU image owner while the real blocking asset read remains pending.
                auto image = camera->image();
                require(image, "late-close image reference");
                retained = std::move(*image);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        if (coordinate_test)
            coordinate_contract &= checksums[0] != checksums[1];
        require(coordinate_test || late_close_test || !has_mesh || checksums[0] != checksums[1],
                "camera/source change alters actual mesh projection");
        require(frame_protocol_checked, "frame ownership negative cases actually executed");
        const auto evidence = renderer->imageEvidence(evidence_image);
        require(evidence && evidence->evidence == rendering::EImageEvidence::GPU_COMPLETE && evidence->frame_serial &&
                    evidence->source == evidence_image.content.source,
                "actual accepted image record reaches the real GPU completion watermark");
        require(evidence_image.content.evidence == rendering::EImageEvidence::REQUESTED,
                "observed completion does not mutate captured descriptors or claim physical screen display");
        if (late_selection_test)
        {
            require(selection_request && held_selection_steps == 32 && provider->entered.load() != 0 &&
                        provider->entered.load() == provider->returned.load(),
                    "all held reads actually returned after 32 selection-independent owner cycles");
            const auto resources = session->readResources();
            require(resources && !session->selection().current, "late adoption does not restore global selection");
            const auto adopted = std::find_if((*resources)->rows.begin(), (*resources)->rows.end(),
                                              [&](const auto &row) { return row.key == *selection_request; });
            require(adopted != (*resources)->rows.end() && adopted->state == sessions::ESceneResourceState::READY,
                    "A completion is adopted using its unchanged Session/entity/source/request identity");
            std::printf("late selection PASS held_cycles=%u reads=%u sequence=%llu selection=empty\n",
                        held_selection_steps, provider->returned.load(), selection_request->sequence);
        }
        if (late_entity_test)
        {
            require(recycled_entity && provider->returned.load() == 1, "late real asset result completed");
            const sessions::SceneOwnerUpdate verify_update{++cycle, 1.0 / 60.0};
            require(session->updateAtOwnerSafePoint(verify_update), "late entity verification read window");
            const auto new_entity = session->readEntity(*recycled_entity);
            require(new_entity && !new_entity->mesh && !session->selection().current,
                    "old asset completion does not write into reused entity slot or restore selection");
            require(!session->readEntity(*original_entity), "old full entity identity remains stale");
            require(session->advanceScene(verify_update), "late entity verification completes owner cycle");
        }
        if (late_source_test)
        {
            require(superseded_key && provider->returned.load() == 1, "old source result actually returned");
            const sessions::SceneOwnerUpdate verify_update{++cycle, 1.0 / 60.0};
            require(session->updateAtOwnerSafePoint(verify_update), "changed source verification read window");
            const auto current = session->readEntity(*original_entity);
            require(current && current->mesh && current->mesh->value.mesh == replacement_mesh &&
                        !session->selection().current,
                    "late old result cannot overwrite changed source or selection");
            const auto stale = session->retryResources(*superseded_key);
            require(!stale && stale.error().code == sessions::ESceneError::STALE_CONTENT,
                    "old source sequence rejected even though full Entity identity remains valid");
            require(session->advanceScene(verify_update), "changed source verification completes owner cycle");
        }
#if defined(LUX_EDITOR_DIAGNOSTICS)
        if (shader_subfailure_test)
        {
            const auto key = sessions::detail::SceneTestAccess::failedShaderKey();
            require(key, "R06 shader rejection issued only after an actual GPU mesh handle was received");
            const sessions::SceneOwnerUpdate update{++cycle, 0};
            require(session->updateAtOwnerSafePoint(update), "R06 final failure snapshot");
            const auto resources = session->readResources();
            require(resources, "R06 public resource failure");
            const auto row = std::find_if((*resources)->rows.begin(), (*resources)->rows.end(),
                                          [&](const auto &value) { return value.key == *key; });
            require(row != (*resources)->rows.end() && row->state == sessions::ESceneResourceState::FAILED &&
                        row->backend_status == 1 && !row->asset_failure && row->render_failure.ok(),
                    "R06 exact ShaderCompiledReply backend status retained with its resource identity");
            const auto outline = session->readOutline();
            require(outline, "R06 authoritative Scene outline");
            const auto entity = std::find_if((*outline)->rows.begin(), (*outline)->rows.end(),
                                             [&](const auto &value) { return value.target == key->target; });
            require(entity != (*outline)->rows.end() && !entity->resources_ready &&
                        sessions::detail::SceneTestAccess::liveResourceHandles(*session, *key) == 0,
                    "R06 failed shader never adopts a partial resolved mesh and releases all sibling handles");
            require(session->advanceScene(update), "R06 complete owner cycle");
            std::printf("R06 PASS mesh_ready_before_shader_failure=1 resource_sequence=%llu backend_status=1 "
                        "remaining_handles_and_requests=0\n",
                        key->sequence);
        }
#endif
        if (resource_backpressure_test && !backpressure_close)
        {
            const auto ready = session->readResources();
            require(ready && backpressured_resources && (*ready)->rows.size() == backpressured_resources->rows.size(),
                    "R09 recovery preserves resource count");
            for (const auto &old : backpressured_resources->rows)
                require(std::any_of((*ready)->rows.begin(), (*ready)->rows.end(),
                                    [&](const auto &row) {
                                        return row.key == old.key && row.state == sessions::ESceneResourceState::READY;
                                    }),
                        "R09 same admitted identities finish actual uploads and render");
            std::puts("R09 recovery complete: same resource keys READY after real Control/Upload saturation");
        }
        if (record_failure_test)
        {
            require(failed_packet_sequence != 0, "failure packet was actually sealed");
            const auto failure_deadline = Clock::now() + std::chrono::seconds{10};
            while (renderer->state() != rendering::ERendererState::FAILED)
            {
                require(Clock::now() < failure_deadline, "accepted record failure finite completion");
                require(renderer->poll(64), "failed backend still permits owner cleanup polling");
                if (pending.valid())
                    require(renderer->trySubmitFrame(pending), "retain fault packet until real admission");
                std::this_thread::yield();
            }
            require(!pending.valid(), "asynchronous record failure does not return an accepted packet");
            require(renderer->poll(64), "consume terminal failure and release accepted references");
            auto diagnostic = renderer->takeDiagnostic();
            require(diagnostic && *diagnostic && (**diagnostic).terminal,
                    "terminal failure has an owning diagnostic record");
            const auto &failure = (**diagnostic).failure;
            require(failure.code == rendering::ERendererError::DEVICE_FAILURE &&
                        failure.render_error.type == injected_error.type &&
                        failure.render_error.args == injected_error.args && failure.request == failed_packet_sequence,
                    "diagnostic preserves exact backend cause and accepted packet identity");
            require(renderer->statistics().accepted_frames == 0, "accepted CPU packets released on terminal failure");
            require(renderer->takeDiagnostic()->has_value() == false, "terminal diagnostic delivered once");
            std::printf("record failure PASS accepted_packet=%llu backend_error=%u:%u\n", failed_packet_sequence,
                        injected_error.type.index, injected_error.type.gen);
        }
        if (churn_test)
        {
            const sessions::SceneOwnerUpdate validation_update{++cycle, 1.0 / 60.0};
            require(session->updateAtOwnerSafePoint(validation_update), "open resource identity validation window");
            require(initial_resources && initial_resources->rows.size() == 3, "owning pre-churn resource snapshot");
            for (const auto &row : initial_resources->rows)
            {
                require(row.state == sessions::ESceneResourceState::READY, "old owning row remains unchanged");
                const auto stale = session->retryResources(row.key);
                require(!stale && stale.error().code == sessions::ESceneError::STALE_CONTENT,
                        "reclaimed request identities are never reused");
            }
            for (std::size_t i = 3; i < checksums.size(); ++i)
                require(checksums[i] == checksums[i % 3], "source rotations restore identical rendered geometry");
            require(session->advanceScene(validation_update), "close resource identity validation window");
        }
        if (dynamic_test)
        {
            require(checksums[1] != checksums[2] && checksums[2] != checksums[3] && checksums[3] == checksums[4],
                    "live light/transform/camera changes then no visuals and empty Scene");
            require((*session->readOutline())->rows.empty() && !session->selection().current,
                    "authoritative clear invalidates selection and publishes empty outline");
        }
        if (retry_test)
        {
            require(retry_accepted && failed_snapshot && failed_key, "failure and explicit retry actually executed");
            require(!session->selection().current, "successful resource adoption never changes selection");
            const auto old = std::find_if(failed_snapshot->rows.begin(), failed_snapshot->rows.end(),
                                          [&](const auto &row) { return row.key == *failed_key; });
            require(old != failed_snapshot->rows.end() && old->asset_failure.has_value(),
                    "old owning failure snapshot survives successful retry");
            std::printf("resource retry PASS old_sequence=%llu original_storage_error=%u\n", failed_key->sequence,
                        unsigned(old->asset_failure->storage_error));
        }
        pending = {};
        if (multiple_views_test)
            require(second_checksums[0] != 0 && second_checksums[0] == second_checksums[1],
                    "first camera changed while second camera output stayed identical");
        if (multiple_lifecycle)
            require(separate_closed && checksums[1] == checksums[2] && checksums[2] == checksums[3],
                    "first target pixels unchanged through second target resize and completed close");
        if (separate_view)
        {
            require(second_checksums[0] != 0 && second_checksums[0] == second_checksums[1],
                    "first camera changed rendered output while second camera output stayed identical");
            require(separate_view->beginClose(), "second view close intent");
        }
        if (variant == "image_lifetime")
        {
            const auto *record = rendering::detail::ViewImageAccess::record(evidence_image);
            const auto render_scene = record->version->scene;
            const auto capture_camera = record->camera;
            retained = {};
            evidence_image = {};
            workspace->releaseFrameImages();
            require(workspace->beginClose(), "retire original workspace before pool experiment");
            while (*workspace->advanceClose() != sessions::ECloseProgress::COMPLETE)
            {
                require(Clock::now() < deadline && renderer->poll(1), "retire workspace in bounded steps");
                std::this_thread::yield();
            }
            workspace.reset();
            auto lifetime_view = renderer->openView(render_scene, {{64, 64}, true});
            require(lifetime_view, "additional real scene view for image lifetime");
            require((*lifetime_view)->setCamera(capture_camera), "capture real scene camera");
            while ((*lifetime_view)->status().state != rendering::EViewState::READY)
            {
                require(Clock::now() < deadline && renderer->poll(1), "prepare image lifetime view");
                std::this_thread::yield();
            }
            exerciseViewLifetime(*renderer, *window, **lifetime_view);
            require((*lifetime_view)->beginClose(), "explicit lifetime view close");
            while (*(*lifetime_view)->advanceClose() != rendering::ERenderClose::COMPLETE)
            {
                require(Clock::now() < deadline && renderer->poll(1), "finish lifetime view close");
                std::this_thread::yield();
            }
            lifetime_view->reset();
        }
        if (variant == "reentrant_close")
        {
            ClosingObserver observer(messages.dispatcherRef());
            observer.window = window.get();
            observer.workspace = workspace.get();
            observer.session = session.get();
            auto connection = session->observe<sessions::SceneSession::selectionChanged, &ClosingObserver::selected,
                                               lux::object::EDelivery::DIRECT>(observer);
            require(connection, "real direct selection connection");
            require(window->beginFrame({{1600, 900}, 1.0F / 60, {1, 1}}), "open frame for callback close");
            require(session->selection().current && session->select(std::nullopt), "publish actual selection change");
            require(observer.calls == 1 && workspace && session, "owners retained after notification returns");
            require(window->discardFrame(), "frame ends before deferred close progression");
            std::puts(
                "selection close PASS: BUSY Session, queued Window/Workspace close, live owners through callback");
        }
        if (workspace)
            require(workspace->beginClose(), "workspace close intent");
        require(session->beginClose(), "Session close intent");
        for (unsigned i = 0; workspace && i < 20; ++i)
        {
            require(renderer->poll(64), "retained image close poll");
            auto closed = workspace->advanceClose();
            require(closed && *closed == sessions::ECloseProgress::PENDING,
                    "CPU image reference retains workspace owner");
            auto session_closed = session->advanceClose();
            require(session_closed && *session_closed == sessions::ECloseProgress::PENDING,
                    "live view retains Session owner");
        }
        retained = {};
        evidence_image = {};
        runtime = {};
        const auto closing_deadline = Clock::now() + std::chrono::seconds{15};
        unsigned held_close_steps{};
        while (workspace || session || separate_view)
        {
            if (Clock::now() >= closing_deadline)
            {
                const auto stats = renderer->statistics();
                std::fprintf(stderr,
                             "close blocked workspace=%d session=%d renderer=%u leases=%zu views=%zu frames=%zu\n",
                             bool(workspace), bool(session), unsigned(renderer->state()), stats.runtime_leases,
                             stats.views, stats.accepted_frames);
            }
            require(Clock::now() < closing_deadline, "finite close deadline");
            require(execution->drainMain(64), "close main continuations");
            require(renderer->poll(64), "close renderer poll");
            if (workspace)
            {
                const auto closed = workspace->advanceClose();
                require(closed, "workspace close step");
                if (*closed == sessions::ECloseProgress::COMPLETE)
                    workspace.reset();
            }
            if (separate_view)
            {
                auto closed = separate_view->advanceClose();
                require(closed, "second view close progression");
                if (*closed == sessions::ECloseProgress::COMPLETE)
                    separate_view.reset();
            }
            if (session)
            {
                const auto closed = session->advanceClose();
                require(closed, "Session close step");
                if (late_close_test && !workspace && !provider->released.load())
                {
                    require(*closed == sessions::ECloseProgress::PENDING && provider->returned.load() == 0,
                            "unfinished blocking operation retains Session after all views close");
                    if (++held_close_steps == 32)
                        provider->release();
                }
                if (*closed == sessions::ECloseProgress::COMPLETE)
                    session.reset();
            }
            std::this_thread::yield();
        }
        require(renderer->beginClose(), "renderer close intent");
        if (late_close_test)
        {
            const auto entered = provider->entered.load();
            const auto returned = provider->returned.load();
            std::printf("late provider entered=%u returned=%u held_close_steps=%u\n", entered, returned,
                        held_close_steps);
            std::fflush(stdout);
            require(held_close_steps == 32 && entered > 0 && returned == entered &&
                        (partial_failure_test || entered == 1),
                    "Session close waits for every actual late provider return");
        }
        if (partial_failure_test)
        {
            require(partial_failure_observed, "a failed sibling did not turn a pending operation into completion");
            std::puts("partial resource failure PASS error retained; live sibling held Session for 32 close steps");
        }
        for (;;)
        {
            require(Clock::now() < closing_deadline, "renderer close deadline");
            require(renderer->poll(64), "renderer drain");
            auto closed = renderer->advanceClose();
            require(closed, "renderer close step");
            if (*closed == rendering::ERenderClose::COMPLETE)
                break;
        }
        require(renderer->joinStopped(), "join proven stopped renderer");
        const auto stats = renderer->statistics();
        require(stats.validation_errors == 0 && stats.texture_misses == 0, "Vulkan validation and texture resolution");
        require(stats.render_events == (variant == "view_failure" ? 2 : (workspace_failure_test ? 1 : 0)) &&
                    stats.dropped_events == (variant == "view_failure" ? 1 : 0),
                "no renderer operation errors beyond explicitly checked view failures");
        require(stats.views == 0 && stats.runtime_leases == 0 && stats.accepted_frames == 0, "terminal owners");
        require(stats.slots == 3 && stats.descriptors_created == stats.descriptors_retired,
                "FIF and descriptor retirement");
        std::printf("ER1 GPU render/retirement subcheck complete variant=%s cycles=%llu frames=%llu "
                    "gpu_completed=%llu descriptors=%llu/%llu\n",
                    argv[3], cycle, stats.frames, stats.gpu_completed, stats.descriptors_created,
                    stats.descriptors_retired);
        renderer.reset();
        require(window->closeAfterRendererStopped(), "window closes after renderer");
        window.reset();
        (*endpoint)->requestStop();
        require((*endpoint)->join(), "asset endpoint terminal join");
        endpoint->reset();
        execution->requestStop();
        require(execution->join(), "execution terminal join");
    }
    lux::meta::ReflectionRegistry::destroyRegistry();
    if (!failed_view_contract)
    {
        std::fputs("G01 FAIL: failed View ordinary operations lost original failure; all owners closed\n", stderr);
        return 1;
    }
    if (!resource_publication_contract)
    {
        std::fputs("G02 FAIL: accepted resource failure missing from retry snapshot; all owners closed\n", stderr);
        return 1;
    }
    if (!coordinate_contract)
    {
        std::fputs("G03 FAIL: Scene and camera wire coordinate pages disagree; all owners closed\n", stderr);
        return 1;
    }
    if (!workspace_failure_contract)
    {
        std::fputs("G04 FAIL: Workspace lost original Scene failure; all owners closed\n", stderr);
        return 1;
    }
    if (!resolved_source_contract)
    {
        std::fputs("R01 FAIL: conflicting resolved source admitted; all owners closed\n", stderr);
        return 1;
    }
    std::printf("ER1 GPU PASS variant=%s all business and shutdown checks complete\n", argv[3]);
    return 0;
}
