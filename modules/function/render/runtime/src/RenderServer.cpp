#include "ServerImpl.hpp"

namespace lux::render
{
    RenderServer::Impl::Impl(
        RenderServerConfig value, ipc::IpcResponder input
    ) : config(std::move(value)), responder(std::move(input))
    {
        addBuiltin<CreateScene, &Impl::createScene>();
        addBuiltin<RemoveScene, &Impl::removeScene>();
        addBuiltin<CreateView, &Impl::createView>();
        addBuiltin<UpdateCamera, &Impl::updateCamera>();
        addBuiltin<RemoveView, &Impl::removeView>();
        addBuiltin<CreateTarget, &Impl::createTarget>();
        addBuiltin<ResizeTarget, &Impl::resizeTarget>();
        addBuiltin<CreateEffect, &Impl::createEffect>();
        addBuiltin<RemoveEffect, &Impl::removeEffect>();
        addBuiltin<Attach, &Impl::attach>();
        addBuiltin<Detach, &Impl::detach>();
        addBuiltin<Readback, &Impl::readback>();
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
        if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++state.validation_errors;
        if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ++state.validation_warnings;
        return VK_FALSE;
    }

    RenderResult<void> RenderServer::Impl::initialize() noexcept
    {
        auto& s = *this;
        auto instance = vulkan::InstanceContext::create({{}, s.config.validation, Impl::diagnostic, &s});
        if (!instance)
        {
            return cxx::unexpected(instance.error());
        }
        s.instance.emplace(std::move(*instance));
        auto options = s.config.device;
        options.host_query_reset |= s.config.uploads && s.config.uploads->measure_gpu;
        auto device = vulkan::DeviceSession::create(*s.instance, options, 3, s.config.retirement_capacity);
        if (!device)
        {
            return cxx::unexpected(device.error());
        }
        s.device = std::move(*device);
        if (s.config.uploads)
        {
            auto uploads = s.device->configureUploads(*s.config.uploads, s.config.cpu);
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
        if (impl_->running.load()) std::terminate(); // host must stop + join
        if (impl_->device) impl_->shutdown();
    }

    RenderResult<std::unique_ptr<RenderServer>> RenderServer::create(RenderServerConfig config) noexcept
    {
        const bool invalid = config.requests_per_tick == 0 || config.max_config_bytes == 0;
        if (invalid) return cxx::unexpected(RenderError{kRuntimeCapacity});
        auto responder = ipc::IpcCore::instance().createResponder(config.channel_name);
        if (!responder) return cxx::unexpected(RenderError{kRuntimeState});
        auto state = std::make_unique<Impl>(std::move(config), std::move(*responder));
        if (auto initialized = state->initialize(); !initialized) return cxx::unexpected(initialized.error());
        return std::unique_ptr<RenderServer>(new RenderServer(std::move(state)));
    }
} // namespace lux::render
