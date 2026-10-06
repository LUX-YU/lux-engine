#include "CompositionProbe.hpp"
#include <array>
#include <cassert>

namespace
{
    using namespace lux;
    using namespace lux::services;
    using namespace lux::editor::desktop;
    class Job final : public fixture::Job
    {
    public:
        Job(object::ObjectDispatcherRef dispatcher, process::ExecutionRuntime& execution, fixture::Trace& trace)
            : fixture::Job(dispatcher), execution_(execution), trace_(trace)
        {
            ++trace_.created;
        }
        ~Job() override
        {
            assert(trace_.owner == std::this_thread::get_id() && trace_.unloaded == 0);
            assert(!task_ && !result_);
            ++trace_.destroyed;
        }
        StartResult start(std::shared_ptr<fixture::Job> owner) noexcept override
        {
            assert(owner.get() == this && !task_ && !result_);
            auto started = execution_.submit(
                {"ec4.external.job"},
                [&](process::TaskReporter) noexcept
                {
                    return stdexec::then(
                        stdexec::schedule(execution_.cpu()),
                        []() noexcept -> cxx::expected<int, int> { return 73; }
                    );
                },
                [this, owner = std::move(owner)](process::TTaskResult<int, int>&& value) noexcept
                {
                    assert(value && trace_.owner == std::this_thread::get_id());
                    result_ = *value;
                    ++trace_.received;
                    assert(emit(changed, *value).complete());
                }
            );
            if (!started)
            {
                return cxx::unexpected(started.error());
            }
            const auto id = started->id();
            trace_.submitted = id;
            task_.emplace(std::move(*started));
            return id;
        }
        std::optional<int> result() const noexcept override
        {
            return result_;
        }
        void acknowledge() noexcept override
        {
            task_.reset();
            result_.reset();
        }
        fixture::Trace& trace() noexcept
        {
            return trace_;
        }

    private:
        process::ExecutionRuntime& execution_;
        fixture::Trace& trace_;
        std::optional<process::Task> task_;
        std::optional<int> result_;
    };
    constexpr std::array contracts{ServiceContract::forType<Job, fixture::Job>(ServiceNameView{"ec4.external.job"})};
    constexpr std::array dependencies{
        ServiceDependency{
            ServiceNameView{"ec4.execution"},
            1,
            cxx::typeToken<process::ExecutionRuntime>(),
            EDependencyKind::BORROWED
        },
        ServiceDependency{ServiceNameView{"ec4.trace"}, 1, cxx::typeToken<fixture::Trace>(), EDependencyKind::BORROWED}
    };
    constexpr auto factory = [](ServiceResolver& resolver,
                                const ServiceConfiguration&) noexcept -> ServiceResult<std::unique_ptr<Job>>
    {
        auto execution = resolver.require<process::ExecutionRuntime>(0);
        if (!execution)
        {
            return cxx::unexpected(std::move(execution.error()));
        }
        auto trace = resolver.require<fixture::Trace>(1);
        if (!trace)
        {
            return cxx::unexpected(std::move(trace.error()));
        }
        return std::make_unique<Job>(resolver.dispatcher(), execution->get(), trace->get());
    };
    constexpr auto definition = []
    {
        auto value = ServiceDescriptor::forType<Job, factory>(
            ServiceNameView{"ec4.external.implementation"},
            contracts,
            dependencies
        );
        value.retention = EServiceRetention::SCOPED;
        value.destroy = [](void* pointer) noexcept
        {
            auto* job = static_cast<Job*>(pointer);
            auto& trace = job->trace();
            delete job;
            assert(trace.unloaded == 0);
            ++trace.destructor_returns;
        };
        return value;
    }();
    class Panel final : public ui::Pane
    {
    public:
        Panel(const UiCreateInfo& input, const char* type, std::shared_ptr<fixture::Job> job, fixture::Trace& trace)
            : Pane(input.dispatcher, input.instance, ui::PaneTypeId{type}, type), job_(std::move(job)), trace_(trace)
        {
            ++trace_.windows;
            if (job_->result())
            {
                assert(*job_->result() == 73);
                ++trace_.shown;
            }
            auto connected = LuxObject::connect(
                job_.get(),
                &fixture::Job::changed,
                this,
                [this](int value) noexcept
                {
                    assert(value == 73);
                    ++trace_.notifications;
                }
            );
            assert(connected);
            connection_ = std::move(*connected);
        }
        ~Panel() override
        {
            assert(trace_.unloaded == 0 && trace_.destroyed == 0);
            ++trace_.windows_destroyed;
        }

        UiResult<lux::editor::workspace::VersionedViewState> captureState() const
        {
            assert(trace_.unloaded == 0);
            ++trace_.operations;
            return lux::editor::workspace::VersionedViewState{1, state_};
        }
        UiStateResult prepareState(const lux::editor::workspace::VersionedViewState& state)
        {
            assert(trace_.unloaded == 0);
            return cxx::move_only_function<void()>{[this, value = state.bytes]() mutable noexcept
            {
                assert(trace_.unloaded == 0);
                state_ = std::move(value);
            }};
        }
    private:
        std::vector<std::byte> state_{std::byte{73}};
        std::shared_ptr<fixture::Job> job_;
        fixture::Trace& trace_;
        object::Connection connection_;
    };
    constexpr std::array ui_dependencies{
        ServiceDependency{ServiceNameView{"ec4.external.job"}, 1, cxx::typeToken<fixture::Job>()},
        dependencies[1]
    };
    UiResult<std::unique_ptr<ui::Pane>> panel(ServiceResolver& resolver, const UiCreateInfo& input, const char* type)
    {
        auto trace = resolver.require<fixture::Trace>(1);
        assert(trace);
        auto job = resolver.get<fixture::Job>(0);
        if (!job)
        {
            return cxx::unexpected(UiFailure{EUiError::DEPENDENCY, "ec4.external.job", 1, job.error().detail});
        }
        return std::make_unique<Panel>(input, type, std::move(*job), trace->get());
    }
    constexpr auto panelDescriptor(const char* type, const char* label, decltype(UiDescriptor::create) create)
    {
        UiDescriptor descriptor{lux::editor::views::ViewTypeIdView{type}, label, ui_dependencies, 1, nullptr, create};
        descriptor.capture_state = [](const ui::Pane& pane) { return static_cast<const Panel&>(pane).captureState(); };
        descriptor.prepare_state = [](ui::Pane& pane, const lux::editor::workspace::VersionedViewState& state)
        { return static_cast<Panel&>(pane).prepareState(state); };
        return descriptor;
    }
    constexpr auto left = panelDescriptor(
        "ec4.external.left", "Left",
        [](ServiceResolver& resolver, const UiCreateInfo& input) { return panel(resolver, input, "ec4.external.left"); }
    );
    constexpr auto right = panelDescriptor(
        "ec4.external.right", "Right",
        [](ServiceResolver& resolver, const UiCreateInfo& input) { return panel(resolver, input, "ec4.external.right"); }
    );

} // namespace
#if defined(_WIN32)
#define FIXTURE_EXPORT __declspec(dllexport)
#else
#define FIXTURE_EXPORT __attribute__((visibility("default")))
#endif
extern "C" FIXTURE_EXPORT void compositionDefinitions(
    lux::object::CodeLease code,
    std::shared_ptr<const lux::services::ServiceEntry>& service,
    std::vector<std::shared_ptr<const lux::editor::desktop::UiEntry>>& ui
)
{
    service = lux::services::ServiceEntry::bind<definition>(code);
    ui.push_back(lux::editor::desktop::UiEntry::bind<left>(code));
    ui.push_back(lux::editor::desktop::UiEntry::bind<right>(std::move(code)));
}
