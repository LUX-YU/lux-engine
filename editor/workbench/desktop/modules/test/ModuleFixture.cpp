#include "ModuleFixture.hpp"
#include "CompositionProbe.hpp"
#include <lux/engine/editor/extensions/EditorExtension.hpp>

extern "C" void
compositionDefinitions(lux::object::CodeLease, std::shared_ptr<const lux::services::ServiceEntry>&, std::vector<std::shared_ptr<const lux::editor::desktop::UiEntry>>&);

namespace module_fixture
{
    namespace
    {
        using namespace lux;
        using namespace lux::editor::commands;
        constexpr services::ServiceDependency dependencies[]{
            {services::ServiceNameView{"ec4.external.job"}, 1, cxx::typeToken<fixture::Job>()}
        };
        CommandResult<std::unique_ptr<CommandBinding>>
        bindCommand(services::ServiceResolver& resolver, const object::CodeLease&) noexcept
        {
            auto job = resolver.get<fixture::Job>(0);
            if (!job)
            {
                return cxx::unexpected(CommandFailure{
                    ECommandError::DOMAIN_FAILURE,
                    "module.service",
                    static_cast<std::uint64_t>(job.error().code)
                });
            }
            return std::make_unique<CommandBinding>(
                [job = *job](const CommandQuery&) -> CommandResult<CommandState>
                { return CommandState{!job->result().has_value()}; },
                [job = *job](const CommandInvocation&) -> CommandResult<DispatchReceipt>
                {
                    auto task = job->start(job);
                    if (!task)
                    {
                        return cxx::unexpected(CommandFailure{
                            ECommandError::DOMAIN_FAILURE,
                            "module.task",
                            static_cast<std::uint64_t>(task.error())
                        });
                    }
                    // This fixture service accepts exactly one operation; TaskId remains owned by Process.
                    return DispatchReceipt{AcceptedOperation{OperationKindId{"module.task"}, 1}};
                }
            );
        }
        constexpr CommandDescriptor command{
            .id = CommandIdView{"ec4.module.start"},
            .label = "Start module task",
            .dependencies = dependencies,
            .create = &bindCommand
        };
    } // namespace
    const lux::editor::extensions::EditorModuleDescriptor& module() noexcept
    {
        using namespace lux::editor::extensions;
        static constexpr EditorModuleDescriptor descriptor{
            "ec4.module.fixture",
            1,
            +[]() noexcept -> const EditorExtensionExports*
            {
                static const EditorExtensionExports exports{
                    .counts = {.commands = 1, .services = 1, .ui = 2},
                    .contribute = +[](ContributionDraft& draft, lux::object::CodeLease code) -> ContributionResult<void>
                    {
                        std::shared_ptr<const lux::services::ServiceEntry> entry;
                        draft.commands.push_back(CommandEntry::bind<command>(code));
                        compositionDefinitions(std::move(code), entry, draft.ui);
                        draft.services.push_back(std::move(entry));
                        return {};
                    }
                };
                return &exports;
            }
        };
        return descriptor;
    }
} // namespace module_fixture
