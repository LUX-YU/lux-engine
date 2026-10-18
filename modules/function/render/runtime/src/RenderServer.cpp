#include "ServerImpl.hpp"
#include <lux/engine/core/execution/ExecutionRuntime.hpp>

namespace lux::render
{
    RenderServer::Impl::Impl(RenderServerConfig value, ipc::IpcResponder input, execution::CpuScheduler host_scheduler)
        : config(std::move(value)), scheduler(std::move(host_scheduler)),
          responder(std::move(input)), render_loop_([](void*) noexcept {
              ipc::IpcCore::instance().notifyWork();
#ifdef LUX_RUNTIME_ASYNC_TEST_SEAM
              runtime_detail::test::completionPublished();
#endif
          })
    {
        addBuiltin<CreateScene, &Impl::createScene>();
        addBuiltin<RemoveScene, &Impl::removeScene>();
        addBuiltin<CreateView, &Impl::createView>();
        addBuiltin<UpdateAttachmentInputs, &Impl::updateAttachmentInputs>();
        addBuiltin<RemoveView, &Impl::removeView>();
        addBuiltin<CreateTarget, &Impl::createTarget>();
        addBuiltin<ResizeTarget, &Impl::resizeTarget>();
        installAsyncBuiltins();
        addBuiltin<RemoveEffect, &Impl::removeEffect>();
        addBuiltin<Attach, &Impl::attachEffect>();
        addBuiltin<Detach, &Impl::detachEffect>();
        addBuiltin<Stop, &Impl::stopCommand>();
        addBuiltin<QueryRenderStats, &Impl::queryRenderStats>();
        addBuiltin<QueryExecutionProgress, &Impl::queryExecutionProgress>();
        addBuiltin<ListFeatures, &Impl::listFeatures>();
        addBuiltin<ListStorages, &Impl::listStorages>();
        addBuiltin<FindStorage, &Impl::findStorage>();
        addBuiltin<PrepareStorage, &Impl::prepareStorage>();
        addBuiltin<ReleasePreparedStorage, &Impl::releasePreparedStorage>();
        addBuiltin<ListCommands, &Impl::listCommands>();
        addBuiltin<ListCommandBindings, &Impl::listCommandBindings>();
        addBuiltin<SelectCommandBinding, &Impl::selectCommandBinding>();
    }

    VKAPI_ATTR VkBool32 VKAPI_CALL RenderServer::Impl::diagnostic(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT,
        const VkDebugUtilsMessengerCallbackDataEXT*,
        void* user
    ) noexcept
    {
        auto& state = *static_cast<Impl*>(user);
        if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        {
            ++state.validation_errors;
        }
        if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        {
            ++state.validation_warnings;
        }
        return VK_FALSE;
    }

    RenderResult<void> RenderServer::Impl::initialize(vulkan::DeviceOptions options) noexcept
    {
        auto& s = *this;
        auto instance = vulkan::InstanceContext::create({{}, s.config.validation, Impl::diagnostic, &s});
        if (!instance)
        {
            return cxx::unexpected(instance.error());
        }
        s.instance.emplace(std::move(*instance));
        options.host_query_reset |= s.config.uploads && s.config.uploads->measure_gpu;
        auto device = vulkan::DeviceSession::create(*s.instance, options, 3, s.config.retirement_capacity);
        if (!device)
        {
            return cxx::unexpected(device.error());
        }
        s.device = std::move(*device);
        if (s.config.uploads)
        {
            auto uploads = s.device->configureUploads(*s.config.uploads, s.scheduler);
            if (!uploads)
            {
                return cxx::unexpected(uploads.error());
            }
        }
        return {};
    }

    RenderServer::RenderServer(std::unique_ptr<Impl> state) noexcept : impl_(std::move(state)) {}

    RenderServer::~RenderServer() noexcept
    {
        if (impl_->running.load())
        {
            std::terminate(); // host must stop + join
        }
        if (impl_->device)
        {
            impl_->shutdown();
        }
    }

    RenderResult<std::unique_ptr<RenderServer>> runtime_detail::ServerBootstrap::create(
        RenderServerConfig config,
        execution::ExecutionRuntime& runtime,
        vulkan::DeviceOptions options
    ) noexcept
    {
        const bool invalid = config.request_work_budget == 0 || config.max_config_bytes == 0;
        if (invalid)
        {
            return cxx::unexpected(RenderError{kRuntimeCapacity});
        }
        auto responder = ipc::IpcCore::instance().createResponder(config.channel_name);
        if (!responder)
        {
            return cxx::unexpected(RenderError{kRuntimeState});
        }
        auto state = std::make_unique<RenderServer::Impl>(std::move(config), std::move(*responder), runtime.cpu());
        if (auto initialized = state->initialize(options); !initialized)
        {
            return cxx::unexpected(initialized.error());
        }
        return std::unique_ptr<RenderServer>(new RenderServer(std::move(state)));
    }
} // namespace lux::render

namespace lux::render::vulkan
{
    RenderResult<std::unique_ptr<RenderServer>> createRenderServer(
        RenderServerConfig config,
        execution::ExecutionRuntime& runtime,
        DeviceOptions options
    ) noexcept
    {
        return runtime_detail::ServerBootstrap::create(std::move(config), runtime, options);
    }
} // namespace lux::render::vulkan
