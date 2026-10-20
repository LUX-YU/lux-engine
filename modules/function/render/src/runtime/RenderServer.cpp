#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/render/runtime/ServerImpl.hpp>

namespace lux::render
{
    RenderServer::Impl::Impl(RenderServerConfig value, ipc::IpcResponder input, execution::CpuScheduler cpu_scheduler)
        : config(std::move(value)), scheduler(std::move(cpu_scheduler)), responder(std::move(input)),
          render_loop_([](void*) noexcept { ipc::IpcCore::instance().notifyWork(); })
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
        addBuiltin<RemoveStorage, &Impl::removeStorage>();
        addBuiltin<ListCommands, &Impl::listCommands>();
        addBuiltin<ListCommandBindings, &Impl::listCommandBindings>();
        addBuiltin<SelectCommandBinding, &Impl::selectCommandBinding>();
    }

    void NRI_CALL
    RenderServer::Impl::diagnostic(nri::Message severity, const char*, std::uint32_t, const char*, void* user) noexcept
    {
        auto& state = *static_cast<Impl*>(user);
        if (severity == nri::Message::ERROR)
        {
            ++state.validation_errors;
        }
        if (severity == nri::Message::WARNING)
        {
            ++state.validation_warnings;
        }
    }

    RenderResult<void> RenderServer::Impl::initialize(DeviceOptions options) noexcept
    {
        auto& s = *this;
        const nri::CallbackInterface diagnostic{Impl::diagnostic, [](void*) noexcept {}, &s};
        auto device =
            detail::DeviceSession::create(options, diagnostic, s.config.validation, 3, s.config.frames_in_flight);
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

    RenderResult<std::unique_ptr<RenderServer>> createRenderServer(
        RenderServerConfig config,
        execution::ExecutionRuntime& runtime,
        DeviceOptions options
    ) noexcept
    {
        const bool invalid =
            config.request_work_budget == 0 || config.max_config_bytes == 0 || config.gpu_wait_ns > 1'000'000;
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
