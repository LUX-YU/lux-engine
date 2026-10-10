#include <lux/engine/RenderContext.hpp>
#include <lux/engine/log/Log.hpp>
#include <lux/engine/process/CompletionWork.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>
#include <lux/engine/scene/RenderResources.hpp>

#include <limits>

namespace lux::engine
{
    struct RenderContext::Impl final
    {
        process::ExecutionRuntime& execution_;
        bool failure_reported_{};
        process::CompletionWork completions_;
        std::shared_ptr<process::CompletionWork::Request> wake_;
        std::unique_ptr<render::RenderRuntime> runtime_;
        process::TaskScope resource_tasks_;
        std::unique_ptr<scene::RenderResources> resources_;

        explicit Impl(process::ExecutionRuntime& execution)
            : execution_(execution),
              completions_(execution, this, [](void* owner) noexcept { static_cast<Impl*>(owner)->complete(); }),
              wake_(std::make_shared<process::CompletionWork::Request>(completions_.requester())),
              resource_tasks_(execution)
        {}

        void complete() noexcept
        {
            auto controls = std::numeric_limits<std::size_t>::max();
            auto programs = controls;
            const auto collected = runtime_->collectCompletions(controls);
            const auto submitted = collected ? runtime_->submitPending(controls, programs)
                                             : render::RenderResult<void>{lux::cxx::unexpected(collected.error())};
            if (!submitted && !std::exchange(failure_reported_, true))
                log::error("render.completions", "Completion dispatch failed ({})", unsigned(submitted.error().code));
        }

        Result registerFeatures(std::vector<render::RenderFeatureRegistration> features) noexcept
        {
            const auto admitted = runtime_->beginFeatureRegistration(std::move(features));
            if (!admitted)
                return lux::cxx::unexpected(VFailure{admitted.error()});
            const auto completed = execution_.waitUntil([&]() noexcept {
                const auto state = runtime_->featureRegistrationStatus().state;
                return state != render::EFeatureRegistrationState::REGISTERING &&
                       state != render::EFeatureRegistrationState::ROLLING_BACK;
            });
            Result result;
            if (completed)
            {
                const auto status = runtime_->featureRegistrationStatus();
                if (status.state != render::EFeatureRegistrationState::READY)
                    return lux::cxx::unexpected(
                        VFailure{render::RendererFailure{render::ERendererError::EXTERNAL_FAILURE, status.error}}
                    );
                const auto committed = runtime_->commitFeatureRegistration();
                if (committed)
                    return {};
                result = lux::cxx::unexpected(VFailure{committed.error()});
            }
            else
                result = lux::cxx::unexpected(VFailure{completed.error()});
            const auto cancelled = runtime_->cancelFeatureRegistration();
            if (!cancelled)
                return lux::cxx::unexpected(VFailure{cancelled.error()});
            const auto rolled_back = execution_.waitUntil([&]() noexcept {
                return runtime_->featureRegistrationStatus().state != render::EFeatureRegistrationState::ROLLING_BACK;
            });
            if (!rolled_back)
                return lux::cxx::unexpected(VFailure{rolled_back.error()});
            return result;
        }
    };

    RenderContext::CreateResult RenderContext::create(
        process::ExecutionRuntime& execution,
        render::RendererConfig configuration
    ) noexcept
    {
        auto impl = std::make_unique<Impl>(execution);
        auto runtime =
            render::RenderRuntime::create(std::move(configuration), [](std::uint32_t severity, std::string_view text) {
                const auto level = severity >= 2   ? log::ELevel::LOG_ERROR
                                   : severity == 1 ? log::ELevel::LOG_WARN
                                                   : log::ELevel::LOG_INFO;
                log::logf(level, "render.validation", "{}", text);
            });
        if (!runtime)
            return lux::cxx::unexpected(VFailure{runtime.error()});
        impl->runtime_ = std::move(*runtime);
        const auto bound = impl->runtime_->bindProgress(impl->wake_, [](void* request) noexcept {
            static_cast<process::CompletionWork::Request*>(request)->request();
        });
        if (!bound)
            return lux::cxx::unexpected(VFailure{bound.error()});
        auto resources = scene::RenderResources::create(*impl->runtime_, impl->resource_tasks_, execution.cpu());
        if (!resources)
            return lux::cxx::unexpected(VFailure{resources.error()});
        impl->resources_ = std::move(*resources);
        return std::unique_ptr<RenderContext>(new RenderContext(std::move(impl)));
    }

    RenderContext::RenderContext(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
    RenderContext::~RenderContext() noexcept = default;
    render::RenderRuntime& RenderContext::runtime() noexcept
    {
        return *impl_->runtime_;
    }
    scene::RenderResources& RenderContext::resources() noexcept
    {
        return *impl_->resources_;
    }
    RenderContext::Result RenderContext::registerFeatures(std::vector<render::RenderFeatureRegistration> features
    ) noexcept
    {
        return impl_->registerFeatures(std::move(features));
    }
}
