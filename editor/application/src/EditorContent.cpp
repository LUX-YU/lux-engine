#include <lux/engine/editor/application/EditorApplicationImpl.hpp>
#include <lux/engine/material/graph/Nodes.hpp>
#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <random>

namespace lux::editor::application
{
    EditorResult<sessions::OpenAssetId> EditorApplication::Impl::createContent(sessions::PreparedSessionData data)
    {
        if (phase_ != EApplicationPhase::RUNNING)
            return cxx::unexpected(EditorFailure{EEditorError::CLOSING, "content.create"});
        if (opens_.size() >= 64)
            return cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "content.create"});
        auto installed = opening_.create(project_->catalogModel().reference({}).project_instance, std::move(data));
        if (!installed)
            return applicationFailure("content.create", installed.error());
        opens_.push_back({*installed});
        return *installed;
    }
    void EditorApplication::Impl::installContentCommands(extensions::ContributionDraft& draft)
    {
        for (bool material : {true, false})
            draft.commands.push_back(std::make_shared<commands::CommandEntry>(
                contracts::CodeLease::builtin(),
                commands::CommandDescriptor{
                    commands::CommandId{material ? "lux.editor.new.material" : "lux.editor.new.flow"},
                    material ? "New Material" : "New Flow",
                    "File"
                },
                [this](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState> {
                    return commands::CommandState{phase_ == EApplicationPhase::RUNNING && opens_.size() < 64};
                },
                [this,
                 material](const commands::CommandInvocation&) -> commands::CommandResult<commands::DispatchReceipt> {
                    std::mt19937 random{std::random_device{}()};
                    auto id = asset::AssetId{uuids::uuid_random_generator{random}()};
                    auto prepared = [&] {
                        if (material)
                        {
                            lux::material::MaterialSource source{id, "Untitled Material", {}};
                            return material::prepareMaterialSession({std::move(source)}, {}, {});
                        }
                        lux::flowforge::FlowSource source;
                        source.id = id;
                        source.name = "Untitled Flow";
                        return flowforge::prepareFlowSession({std::move(source)}, {}, {}, flow_environment_);
                    }();
                    auto opened = createContent(std::move(prepared));
                    if (!opened)
                        return cxx::unexpected(commands::CommandFailure{
                            commands::ECommandError::DOMAIN_FAILURE,
                            opened.error().domain,
                            opened.error().reason,
                            opened.error().message
                        });
                    return commands::DispatchReceipt{commands::AcceptedOperation{"open", opened->value}};
                }
            ));
    }
}
