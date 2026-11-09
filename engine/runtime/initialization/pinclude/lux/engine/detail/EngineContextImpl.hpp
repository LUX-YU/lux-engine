#pragma once

#include <lux/engine/EngineContext.hpp>
#include <lux/engine/assets/AssetRepository.hpp>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/object/ObjectRuntime.hpp>
#include <lux/engine/resource/asset/storage/AssetVfs.hpp>

#if defined(LUX_ENGINE_WITH_RENDER)
#include <lux/engine/systems/render/RenderFrameSubmission.hpp>
#include <lux/engine/render/runtime/RenderClient.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/core/execution/CompletionWork.hpp>
#include <optional>
#include <thread>
#endif

namespace lux::engine
{
    struct EngineContext::Impl final
    {
        explicit Impl(std::unique_ptr<execution::ExecutionRuntime> execution) noexcept;
        ~Impl() noexcept;

        // Execution is constructed first and remains dispatchable through every consumer's shutdown.
        std::unique_ptr<execution::ExecutionRuntime> execution;
        asset::AssetVfs asset_vfs;
        std::unique_ptr<object::ObjectRuntime> objects;
#if defined(LUX_ENGINE_WITH_RENDER)
        std::unique_ptr<lux::render::RenderServer> server;
        std::unique_ptr<lux::render::RenderClient> render_client;
        std::optional<execution::CompletionWork> render_responses;
        std::thread render_thread;
        std::optional<render::RenderFrameSubmission> frames;
        bool stopping_render = false;

        static void notifyResponses(void* context) noexcept;
        void dispatchRenderResponses() noexcept;
        void runRender() noexcept;
#endif
        // Repository use release may still send commands; it must settle before the Render service stops.
        std::unique_ptr<assets::AssetRepository> assets;
    };
} // namespace lux::engine
