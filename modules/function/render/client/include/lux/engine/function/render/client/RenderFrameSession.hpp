#pragma once
/**
 * @file RenderFrameSession.hpp
 * @brief Main-thread endpoint for one lexical FrameProgram transaction.
 *
 * This type deliberately has no scene/view/target/resource lifecycle API.
 * Those operations belong to RenderControlSession and RenderUploadSession and
 * remain valid while no frame is open. A RenderFrameSession only brackets and
 * records frame-local work.
 */

#include <lux/engine/function/visibility.h>
#include <lux/engine/function/render/client/RenderProtocol.hpp>
#include <lux/engine/function/render/client/RenderClient.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace lux::render
{
    class LUX_FUNCTION_PUBLIC RenderFrameSession
    {
    public:
        using Client  = GeneralRenderClient;
        using Builder = Client::Builder;
        using FrameProgressToken = Client::FrameProgressToken;

        explicit RenderFrameSession(
            std::shared_ptr<RenderFrameChannel<>> channel,
            std::shared_ptr<RenderChannelSync> sync
        );

        RenderFrameSession(const RenderFrameSession&) = delete;
        RenderFrameSession& operator=(const RenderFrameSession&) = delete;

        std::size_t pumpReplies();
        [[nodiscard]] bool waitAndPumpReplies();

        void setErrorEventHandler(
            std::function<void(const ErrorEventBatchReply&)> on_batch,
            std::function<void(const RenderErrorEvent&)> on_event
        );
        [[nodiscard]] std::uint64_t unroutedUnsolicitedReplies() const noexcept;

        [[nodiscard]] bool beginFrame(const FrameMemoryHints& hints = {});
        [[nodiscard]] bool isRecording() const noexcept
        {
            return client_.isRecording();
        }
        [[nodiscard]] bool trySubmitFrame() noexcept;
        [[nodiscard]] FrameProgressToken observeProgress() const noexcept;
        void waitForProgress(FrameProgressToken observed) const noexcept;
        [[nodiscard]] bool waitForProgressUntil(
            FrameProgressToken observed,
            std::chrono::steady_clock::time_point deadline) const noexcept;
        void notifyProgress() noexcept;
        [[nodiscard]] bool isStopping() const noexcept;
        [[nodiscard]] RenderError terminalError() const noexcept;
        [[nodiscard]] std::shared_ptr<RenderChannelSync>
        progressDomain() const noexcept;

        [[nodiscard]] Builder& builder() noexcept;
        [[nodiscard]] bool rebaseSceneOrigin(
            RenderSceneId scene,
            const std::int64_t scene_origin_page[3]) noexcept;
        [[nodiscard]] Client& rawClient() noexcept { return client_; }
        [[nodiscard]] const Client& rawClient() const noexcept { return client_; }

        void requestStop() noexcept;

    private:
        Client client_;
    };
} // namespace lux::render
