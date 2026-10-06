#include <array>
#include <lux/engine/editor/launcher/LaunchEditor.hpp>
#include <lux/engine/platform/Process.hpp>
#include <lux/engine/process/TaskScope.hpp>
#include <lux/engine/services/ServiceRegistry.hpp>
#include <thread>

namespace lux::editor
{
    EditorResult<void> launchEditor(
        const std::filesystem::path& installation,
        const std::filesystem::path& project_file
    ) noexcept
    {
        const auto encoded = project_file.u8string();
        const std::array arguments{std::string{"--project"}, std::string(encoded.begin(), encoded.end())};
        auto executable = engine::platform::executablePath();
        if (!executable)
        {
            return lux::cxx::unexpected(
                EditorFailure{EEditorError::SOURCE_FAILURE, "editor.executable", executable.error().native_code}
            );
        }
        auto editor = installation / "bin/lux_editor_legacy";
        editor += executable->extension();
        const auto launched = engine::platform::launchProcess(editor, arguments);
        if (!launched)
        {
            return lux::cxx::unexpected(EditorFailure{
                EEditorError::SOURCE_FAILURE,
                "editor.launch",
                launched.error().native_code,
                "The project remains saved. Check the Editor installation and retry opening it."
            });
        }
        return {};
    }
    struct ProjectLaunching::Impl final
    {
        process::TaskScope tasks_;
        std::filesystem::path installation_;
        std::optional<process::TaskId> pending_;
        std::optional<EditorResult<void>> result_;
        const std::thread::id owner_{std::this_thread::get_id()};

        Impl(process::ExecutionRuntime& execution, std::filesystem::path installation)
            : tasks_(execution), installation_(std::move(installation))
        {
        }
        ~Impl()
        {
            // The normal loop receives completion first. Construction/owner cleanup still drains
            // the original TaskScope before destroying the storage captured by its receiver.
            if (!tasks_.join())
            {
                std::terminate();
            }
        }
        EditorResult<void> admission() const
        {
            if (owner_ != std::this_thread::get_id())
            {
                return cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "project.launch.thread"});
            }
            return {};
        }
        EditorResult<void> request(std::filesystem::path file)
        {
            if (auto admitted = admission(); !admitted)
            {
                return admitted;
            }
            const bool has_accepted_work = pending_.has_value() || result_.has_value();
            if (has_accepted_work)
            {
                return cxx::unexpected(EditorFailure{EEditorError::BUSY, "project.launch"});
            }
            auto blocking = tasks_.execution().blocking();
            if (!blocking)
            {
                return cxx::unexpected(
                    EditorFailure{EEditorError::EXECUTION_FAILURE, "project.launch.scheduler", 0, {}, blocking.error()}
                );
            }
            auto accepted = tasks_.submit(
                {"Open project in Editor", "Project"},
                [file = std::move(file), installation = installation_, scheduler = *blocking](process::TaskReporter
                ) noexcept
                {
                    return stdexec::then(
                        stdexec::schedule(scheduler),
                        [file, installation]() noexcept { return launchEditor(installation, file); }
                    );
                },
                [this](process::TTaskResult<void, EditorFailure>&& completed) noexcept
                {
                    pending_.reset();
                    if (completed)
                    {
                        result_.emplace();
                    }
                    else if (auto* error = completed.error().domainFailure())
                    {
                        result_.emplace(cxx::unexpected(std::move(*error)));
                    }
                    else
                    {
                        result_.emplace(cxx::unexpected(EditorFailure{
                            EEditorError::EXECUTION_FAILURE,
                            "project.launch.task",
                            0,
                            {},
                            completed.error()
                        }));
                    }
                }
            );
            if (!accepted)
            {
                return cxx::unexpected(
                    EditorFailure{EEditorError::EXECUTION_FAILURE, "project.launch.submit", 0, {}, accepted.error()}
                );
            }
            pending_ = *accepted;
            return {};
        }
    };
    ProjectLaunching::ProjectLaunching(process::ExecutionRuntime& execution, std::filesystem::path installation)
        : impl_(std::make_unique<Impl>(execution, std::move(installation)))
    {
    }
    ProjectLaunching::~ProjectLaunching() = default;
    EditorResult<void> ProjectLaunching::request(std::filesystem::path file)
    {
        return impl_->request(std::move(file));
    }
    bool ProjectLaunching::pending() const noexcept
    {
        return impl_->pending_.has_value();
    }
    const EditorResult<void>* ProjectLaunching::result() const noexcept
    {
        return impl_->result_ ? &*impl_->result_ : nullptr;
    }
    EditorResult<void> ProjectLaunching::acknowledge()
    {
        if (auto admitted = impl_->admission(); !admitted)
        {
            return admitted;
        }
        if (impl_->pending_)
        {
            return cxx::unexpected(EditorFailure{EEditorError::BUSY, "project.launch"});
        }
        if (!impl_->result_)
        {
            return cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "project.launch"});
        }
        impl_->result_.reset();
        return {};
    }
    namespace
    {
        constexpr services::ServiceContract contracts[]{
            services::ServiceContract::forType<ProjectLaunching, ProjectLaunching>(
                services::ServiceNameView{"lux.editor.project.launching"}
            )
        };
        constexpr services::ServiceDependency dependencies[]{
            {services::ServiceNameView{"lux.process.execution"},
             1,
             cxx::typeToken<process::ExecutionRuntime>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.editor.installation"},
             1,
             cxx::typeToken<std::filesystem::path>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT}
        };
        services::ServiceResult<std::unique_ptr<ProjectLaunching>>
        createLaunching(services::ServiceResolver& resolver, const services::ServiceConfiguration&) noexcept
        {
            auto execution = resolver.require<process::ExecutionRuntime>(0);
            if (!execution)
            {
                return cxx::unexpected(std::move(execution.error()));
            }
            auto installation = resolver.require<std::filesystem::path>(1);
            if (!installation)
            {
                return cxx::unexpected(std::move(installation.error()));
            }
            return std::make_unique<ProjectLaunching>(execution->get(), installation->get());
        }
    } // namespace
    constinit const services::ServiceDescriptor kProjectLaunchingService = []
    {
        auto descriptor = services::ServiceDescriptor::forType<ProjectLaunching, createLaunching>(
            services::ServiceNameView{"lux.editor.project.launching"},
            contracts,
            dependencies
        );
        descriptor.affinity = services::EServiceAffinity::OWNER;
        descriptor.retention = services::EServiceRetention::SCOPED;
        descriptor.settled = [](const void* value) noexcept -> services::ServiceResult<bool>
        { return !static_cast<const ProjectLaunching*>(value)->pending(); };
        return descriptor;
    }();
} // namespace lux::editor
