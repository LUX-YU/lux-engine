#pragma once
/**
 * @file RenderBackendHost.hpp
 * @brief Process-level render channel and thread owner (architecture design
 *        §2.4; 装配归属 ADR 裁决三): spawn the server thread, bring the
 *        server up ON that thread, register the standard feature plan before
 *        ready flips, then serve tick()s until the channel stops.
 *
 * One template, every host instantiates it:
 *   - `RenderBackendHost<>` (GeneralRenderServer) — desktop GameHost + the
 *     Android game shell. The built-in default bring-up applies (ServerConfig
 *     from the three common fields + init(); NO attachToWindow — surfaces
 *     arrive later through the command path), and start() additionally builds
 *     the client RenderFrameSession. FrameCoordinator alone opens lexical
 *     frames after start() returns.
 *   - `RenderBackendHost<lux::ui::UIRenderServer>` — editor + launcher. The
 *     host passes `bring_up` (UIRenderServer + ImGui font atlas +
 *     attachToWindow, run ON the render thread) and `post_init` (grab the
 *     ImGui op-ids before ready flips), then builds its own UIRenderFrameSession
 *     from channel()/sync() — the UI session shape (ImGui ops, no frame open
 *     at start) is the host's, not this class's.
 *
 * What the host still owns: WHERE validation text lands (the sink in the
 * config), HOW its server comes up (bring_up), targets, content, the main
 * loop. This class owns the thread, all three channels/sessions and the
 * accepted-work close protocol. start() blocks until the server is up or
 * failed; stop() closes admission, pumps terminal replies while the render
 * owner drains accepted work, joins, and returns a structured report.
 */

#include <lux/engine/runtime/render/scene/StandardFeaturePlan.hpp>   // registerStandardRenderFeatures
#include <lux/engine/runtime/render/scene/RenderDiagnostics.hpp>     // installRenderErrorLogging / reportUnroutedRenderReplies
#include <lux/engine/runtime/render/scene/BuiltinRenderEffects.hpp>
#include <lux/engine/runtime/extensions/RenderEffects.hpp>

#include <lux/engine/function/render/client/FeatureCatalog.hpp>
#include <lux/engine/function/render/client/RenderControlSession.hpp>
#include <lux/engine/function/render/client/RenderUploadSession.hpp>
#include <lux/engine/function/render/client/RenderFrameSession.hpp>   // RenderFrameChannel<> / RenderChannelSync
#include <lux/engine/render/comm/server/RenderServer.hpp>    // GeneralRenderServer / ServerConfig

#include <lux/engine/log/Log.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <concepts>
#include <functional>
#include <memory>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace lux::runtime
{
    struct RenderThreadConfig
    {
        std::vector<const char*> instance_extensions;
        bool                     enable_validation = false;
        lux::render::CapacityRequest capacity_request{};
        lux::render::CapacityPlan* capacity_plan_output{nullptr};
        lux::render::CapacityShortfall* capacity_shortfall_output{
            nullptr};
        std::atomic<int>* validation_error_counter{nullptr};
        decltype(lux::render::ServerConfig{}.validation_message_sink)
                                 validation_message_sink;
    };

    struct RenderBackendCloseReport
    {
        lux::render::UploadLifecycleSnapshot uploads{};
        std::size_t pending_frame_requests{0};
        std::size_t pending_frame_replies{0};
        std::size_t pending_control_requests{0};
        std::size_t pending_control_replies{0};
        std::size_t pending_upload_requests{0};
        std::size_t pending_upload_replies{0};
        std::size_t upload_queue_high_water{0};
        std::size_t upload_payload_high_water{0};

        [[nodiscard]] bool clean() const noexcept
        {
            return uploads.clean() && pending_frame_requests == 0 &&
                pending_frame_replies == 0 &&
                pending_control_requests == 0 &&
                pending_control_replies == 0 &&
                pending_upload_requests == 0 &&
                pending_upload_replies == 0;
        }
    };

    struct RenderBackendEndpoints
    {
        std::shared_ptr<lux::render::RenderFrameChannel<>> frame;
        std::shared_ptr<lux::render::RenderControlChannel<>> control;
        std::shared_ptr<lux::render::RenderUploadChannel<>> upload;
        std::shared_ptr<lux::render::RenderChannelSync> sync;

        [[nodiscard]] static RenderBackendEndpoints create()
        {
            return {
                lux::render::RenderFrameChannel<>::create(),
                lux::render::RenderControlChannel<>::create(),
                lux::render::RenderUploadChannel<>::create(),
                std::make_shared<lux::render::RenderChannelSync>()
            };
        }

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return frame && control && upload && sync;
        }
    };

    enum class ERenderBackendStartState : std::uint8_t
    {
        IDLE,
        STARTING,
        READY,
        FAILED
    };

    template <class ServerT = lux::render::GeneralRenderServer>
    class RenderBackendHost
    {
    public:
        struct Config : RenderThreadConfig
        {
            std::function<std::unique_ptr<ServerT>(
                const std::shared_ptr<lux::render::RenderFrameChannel<>>&,
                const std::shared_ptr<lux::render::RenderControlChannel<>>&,
                const std::shared_ptr<lux::render::RenderUploadChannel<>>&,
                const std::shared_ptr<lux::render::RenderChannelSync>&,
                const RenderThreadConfig&)>
                bring_up;

            std::function<void(ServerT&)> post_init;
        };

        RenderBackendHost() = default;
        ~RenderBackendHost() { (void)stop(); }

        RenderBackendHost(const RenderBackendHost&)            = delete;
        RenderBackendHost& operator=(const RenderBackendHost&) = delete;

        [[nodiscard]] bool start(Config cfg)
        {
            if (endpoints_)
                return true;

            if (!cfg.bring_up)
            {
                if constexpr (std::same_as<ServerT, lux::render::GeneralRenderServer>)
                {
                    cfg.bring_up =
                        [](const std::shared_ptr<lux::render::RenderFrameChannel<>>& channel,
                           const std::shared_ptr<lux::render::RenderControlChannel<>>& control_channel,
                           const std::shared_ptr<lux::render::RenderUploadChannel<>>& upload_channel,
                           const std::shared_ptr<lux::render::RenderChannelSync>&      sync,
                           const RenderThreadConfig& c)
                        -> std::unique_ptr<lux::render::GeneralRenderServer>
                    {
                        auto server = std::make_unique<lux::render::GeneralRenderServer>(
                            channel, control_channel, upload_channel, sync);
                        lux::render::ServerConfig scfg;
                        scfg.instance_extensions     = c.instance_extensions;
                        scfg.enable_validation       = c.enable_validation;
                        scfg.capacity_request        = c.capacity_request;
                        scfg.capacity_shortfall_output =
                            c.capacity_shortfall_output;
                        scfg.validation_error_counter =
                            c.validation_error_counter;
                        scfg.validation_message_sink = c.validation_message_sink;
                        if (auto r = server->init(std::move(scfg)); !r)
                            return nullptr;
                        if (c.capacity_plan_output)
                            *c.capacity_plan_output = server->capacityPlan();
                        return server;
                    };
                }
                else
                {
                    lux::log::error("render",
                        "RenderBackendHost: no bring_up callback and no default "
                        "bring-up for this server type -- start() refused");
                    return false;
                }
            }

            endpoints_ = RenderBackendEndpoints::create();
            thread_done_.store(false, std::memory_order_release);
            start_state_.store(
                ERenderBackendStartState::STARTING,
                std::memory_order_release
            );
            close_report_ = {};

            thread_ = std::thread([this, cfg = std::move(cfg)]
            {
                auto server = cfg.bring_up(
                    endpoints_.frame, endpoints_.control, endpoints_.upload, endpoints_.sync, cfg);
                if (!server)
                {
                    start_state_.store(
                        ERenderBackendStartState::FAILED,
                        std::memory_order_release
                    );
                    start_state_.notify_one();
                    thread_done_.store(true, std::memory_order_release);
                    thread_done_.notify_all();
                    return;
                }
                registerStandardRenderFeatures(*server, feature_catalog_,
                                               feature_plan_);
                if (cfg.post_init)
                    cfg.post_init(*server);
                start_state_.store(
                    ERenderBackendStartState::READY,
                    std::memory_order_release
                );
                start_state_.notify_one();
                while (server->tick()) {}
                close_report_.uploads = server->closeAcceptedUploads();
                thread_done_.store(true, std::memory_order_release);
                thread_done_.notify_all();
                endpoints_.sync->notifyReplyProduced();
            });

            auto state = start_state_.load(std::memory_order_acquire);
            while (state == ERenderBackendStartState::STARTING)
            {
                start_state_.wait(state, std::memory_order_acquire);
                state = start_state_.load(std::memory_order_acquire);
            }
            if (state == ERenderBackendStartState::FAILED)
            {
                if (thread_.joinable())
                    thread_.join();
                endpoints_ = {};
                start_state_.store(
                    ERenderBackendStartState::IDLE,
                    std::memory_order_release
                );
                return false;
            }

            control_session_ =
                std::make_unique<lux::render::RenderControlSession>(
                    endpoints_.control, endpoints_.sync);
            upload_session_ =
                std::make_unique<lux::render::RenderUploadSession>(
                    endpoints_.upload, endpoints_.sync);
            render_effect_types_ =
                std::make_unique<RenderEffectTypeRegistry>(
                    *control_session_);
            if (!render_effect_catalog_.find(
                    lux::extensions::contributionId(
                        "org.lux.render.grid3d.effect")))
            {
                const auto added = addGrid3DRenderEffect(
                    render_effect_catalog_);
                if (!added)
                {
                    lux::log::error(
                        "render",
                        "RenderBackendHost: failed to register the built-in "
                        "Grid3D render-effect contribution");
                    (void)stop();
                    return false;
                }
            }

            if constexpr (std::same_as<ServerT, lux::render::GeneralRenderServer>)
            {
                session_ = std::make_unique<lux::render::RenderFrameSession>(endpoints_.frame, endpoints_.sync);
            }
            return true;
        }

        [[nodiscard]] RenderBackendCloseReport stop() noexcept
        {
            if (endpoints_.sync)
            {
                endpoints_.sync->requestStop();
                while (!thread_done_.load(std::memory_order_acquire))
                {
                    if (session_)
                        session_->pumpReplies();
                    if (control_session_)
                        control_session_->pumpReplies();

                    const auto observed = endpoints_.sync->reply_epoch.load(
                        std::memory_order_acquire
                    );
                    if (thread_done_.load(std::memory_order_acquire))
                        break;
                    endpoints_.sync->reply_epoch.wait(
                        observed,
                        std::memory_order_acquire
                    );
                }
            }
            if (thread_.joinable())
                thread_.join();
            if (session_)
                session_->pumpReplies();
            if (control_session_)
                control_session_->pumpReplies();
            if (session_)
                reportUnroutedRenderReplies(*session_);
            if (render_effect_types_)
            {
                render_effect_types_->clear();
                render_effect_types_.reset();
            }
            if (endpoints_.frame)
            {
                close_report_.pending_frame_requests =
                    endpoints_.frame->requests.pendingFrames();
                close_report_.pending_frame_replies =
                    endpoints_.frame->responses.pendingFrames();
            }
            if (endpoints_.control)
            {
                close_report_.pending_control_requests =
                    endpoints_.control->requests.size();
                close_report_.pending_control_replies =
                    endpoints_.control->responses.pendingFrames();
            }
            if (endpoints_.upload)
            {
                close_report_.pending_upload_requests =
                    endpoints_.upload->requests.size();
                close_report_.pending_upload_replies =
                    endpoints_.upload->responses.pendingFrames();
                close_report_.upload_queue_high_water =
                    endpoints_.upload->queueHighWater();
                close_report_.upload_payload_high_water =
                    endpoints_.upload->payloadHighWater();
            }
            auto endpoints = std::exchange(endpoints_, {});
            auto sessions = std::tuple{
                std::move(session_),
                std::move(control_session_),
                std::move(upload_session_)
            };
            start_state_.store(
                ERenderBackendStartState::IDLE,
                std::memory_order_release
            );
            return close_report_;
        }

        [[nodiscard]] lux::render::RenderFrameSession& session() noexcept
            requires std::same_as<ServerT, lux::render::GeneralRenderServer>
        { return *session_; }

        [[nodiscard]] lux::render::RenderControlSession& controlSession() noexcept
        { return *control_session_; }

        [[nodiscard]] lux::render::RenderUploadSession& uploadSession() noexcept
        { return *upload_session_; }

        [[nodiscard]] const std::shared_ptr<lux::render::RenderFrameChannel<>>&
        channel() const noexcept { return endpoints_.frame; }
        [[nodiscard]] const std::shared_ptr<lux::render::RenderControlChannel<>>&
        controlChannel() const noexcept { return endpoints_.control; }
        [[nodiscard]] const std::shared_ptr<lux::render::RenderUploadChannel<>>&
        uploadChannel() const noexcept { return endpoints_.upload; }
        [[nodiscard]] const std::shared_ptr<lux::render::RenderChannelSync>&
        sync() const noexcept { return endpoints_.sync; }

        [[nodiscard]] lux::render::FeatureCatalog& featureCatalog() noexcept
        {
            return feature_catalog_;
        }
        [[nodiscard]] const lux::render::FeatureCatalog& featureCatalog()
            const noexcept
        {
            return feature_catalog_;
        }
        [[nodiscard]] const std::vector<lux::render::FeatureAttach>&   featurePlan()     const noexcept { return feature_plan_; }
        [[nodiscard]] RenderEffectCatalog& renderEffectCatalog()
            noexcept
        {
            return render_effect_catalog_;
        }
        [[nodiscard]] RenderEffectTypeRegistry& renderEffectTypes()
            noexcept
        {
            return *render_effect_types_;
        }

    private:
        RenderBackendEndpoints                              endpoints_{};
        std::thread                                          thread_;
        std::atomic<bool>                                    thread_done_{true};
        std::atomic<ERenderBackendStartState>                start_state_{
            ERenderBackendStartState::IDLE};
        RenderBackendCloseReport                             close_report_{};
        std::unique_ptr<lux::render::RenderFrameSession>          session_;   // GeneralRenderServer branch only
        std::unique_ptr<lux::render::RenderControlSession>   control_session_;
        std::unique_ptr<lux::render::RenderUploadSession>    upload_session_;
        lux::render::FeatureCatalog                          feature_catalog_;
        std::vector<lux::render::FeatureAttach>              feature_plan_;
        RenderEffectCatalog                                  render_effect_catalog_;
        std::unique_ptr<RenderEffectTypeRegistry>             render_effect_types_;
    };

} // namespace lux::runtime
