#include "Model.hpp"
#include "Probe.hpp"
#include <lux/engine/editor/extensions/EditorExtension.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/Controls.hpp>

namespace skeleton
{
    using namespace sessions;
    using namespace extensions;
    namespace
    {
        Facts *facts{};
        SessionStore *observed_store{}; // Test-only observation; production closures borrow explicit activation inputs.
        struct Unload final
        {
            ~Unload()
            {
                if (facts)
                    ++facts->unloaded;
            }
        } unload;
        class Window final : public ui::Pane
        {
        public:
            Window(const Window &) = delete;
            Window &operator=(const Window &) = delete;
            Window(Window &&) = delete;
            Window &operator=(Window &&) = delete;
            Window(const views::ViewFactoryInput &input, TSessionAccess<Session> access, views::ViewContent content)
                : Pane(input.dispatcher(), input.paneId(), ui::PaneTypeId{"example.skeleton.view"}, "Skeleton"),
                  access_(access), content_(std::move(content)),
                  layout_(*this, ui::ElementId{"layout"}, ui::ELayoutType::VERTICAL),
                  bones_(layout_, ui::ElementId{"bones"}), name_(layout_, ui::ElementId{"root.name"}),
                  translation_(layout_, ui::ElementId{"global.x"}, 0.0F),
                  apply_(layout_, ui::ElementId{"apply"}, "Apply root name and global X"),
                  revert_(layout_, ui::ElementId{"revert"}, "Revert"), error_(layout_, ui::ElementId{"error"})
            {
                setContent(layout_);
                auto apply = object::LuxObject::connect(&apply_, &ui::Button::activated, this, &Window::queueApply);
                if (!apply)
                    std::terminate();
                apply_connection_ = std::move(*apply);
                auto revert = object::LuxObject::connect(&revert_, &ui::Button::activated, this, &Window::queueRevert);
                if (!revert)
                    std::terminate();
                revert_connection_ = std::move(*revert);
                if (facts)
                    ++facts->panes_created;
                update();
            }
            ~Window() override
            {
                if (facts)
                    ++facts->panes_destroyed;
            }
            views::ViewContent content() const noexcept { return content_; }
            views::ViewCloseResult bind(const views::ViewContent &content)
            {
                if (pending_)
                    return cxx::unexpected(views::ViewPreparationFailure{"skeleton.draft", 0, "Apply pending", true});
                if (!content.primary)
                    return cxx::unexpected(views::ViewPreparationFailure{"skeleton.binding", 0, "No content", false});
                auto key = access_.key(*content.primary);
                if (!key)
                    return cxx::unexpected(
                        views::ViewPreparationFailure{"skeleton.binding", 0, "Stale content", false});
                content_ = content;
                draft_.reset();
                return {};
            }

        private:
            struct Draft final
            {
                ContentStamp based_on;
                std::string name;
                float x{};
            };
            void queueApply() noexcept
            {
                if (draft_)
                    pending_ = Draft{draft_->based_on, name_.value(), std::get<float>(translation_.value())};
            }
            void queueRevert() noexcept { revert_requested_ = true; }
            void update() noexcept override
            {
                if (!content_.primary)
                    return;
                auto key = access_.key(*content_.primary);
                if (!key)
                    return;
                auto owner = access_.edit(*key);
                if (!owner)
                    return;
                if (revert_requested_)
                {
                    pending_.reset();
                    draft_.reset();
                    error_.setText({});
                    revert_requested_ = false;
                }
                if (pending_)
                {
                    auto applied = owner->get().edit(pending_->based_on, 0, pending_->name, pending_->x);
                    if (!applied)
                    {
                        error_.setText(applied.error() == ESessionError::BUSY ? "Busy; draft retained"
                                                                              : "Source changed; Revert required");
                        return;
                    }
                    pending_.reset();
                    draft_.reset();
                }
                if (draft_)
                    return;
                auto value = owner->get().read();
                if (!value || value->bones.empty())
                    return;
                const auto stamp = owner->get().describe().current;
                std::string rows;
                for (std::size_t i{}; i != value->bones.size(); ++i)
                    rows += std::to_string(i) + ": " + value->bones[i].name + " (parent " +
                            std::to_string(value->bones[i].parent_index) + ")\n";
                bones_.setText(std::move(rows));
                if (facts)
                    facts->rows_prepared += static_cast<unsigned>(value->bones.size());
                draft_ = Draft{stamp, value->bones[0].name, value->global_transform.translation().x()};
                name_.setValue(draft_->name);
                translation_.setValue(draft_->x);
            }
            TSessionAccess<Session> access_;
            views::ViewContent content_;
            ui::Layout layout_;
            ui::Label bones_;
            ui::TextEdit name_;
            ui::NumericEdit translation_;
            ui::Button apply_, revert_;
            ui::Label error_;
            object::Connection apply_connection_, revert_connection_;
            std::optional<Draft> draft_, pending_;
            bool revert_requested_{};
        };
        ContributionResult<void> contribute(ContributionDraft &draft, contracts::CodeLease code)
        {
            draft.sessions.push_back(factory(std::move(code)));
            return {};
        }
        ContributionResult<void> activate(ContributionDraft &draft,
                                          contracts::CodeLease code,
                                          const ExtensionCapabilities &capabilities)
        {
            observed_store = &capabilities.sessions->sessions;
            if (facts)
            {
                ++facts->activations;
            }
            const auto access = observed_store->access<Session>();
            draft.commands.push_back(std::make_shared<commands::CommandEntry>(
                code,
                commands::CommandDescriptor{commands::CommandId{"example.skeleton.rename"},
                                            "Edit skeleton",
                                            "Skeleton",
                                            {},
                                            commands::ECommandScope::SESSION,
                                            1,
                                            cxx::typeToken<Rename>()},
                [access](const commands::CommandQuery &input) -> commands::CommandResult<commands::CommandState>
                {
                    auto key = access.key(std::get<commands::SessionTarget>(input.target).id);
                    if (!key)
                        return cxx::unexpected(commands::CommandFailure{commands::ECommandError::STALE_TARGET});
                    return commands::CommandState{true};
                },
                [access](const commands::CommandInvocation &input) -> commands::CommandResult<commands::DispatchReceipt>
                {
                    const auto &target = std::get<commands::SessionTarget>(input.target());
                    if (!target.based_on)
                        return cxx::unexpected(commands::CommandFailure{commands::ECommandError::INVALID_ARGUMENT});
                    auto key = access.key(target.id);
                    if (!key)
                        return cxx::unexpected(commands::CommandFailure{commands::ECommandError::STALE_TARGET});
                    auto owner = access.edit(*key);
                    if (!owner)
                        return cxx::unexpected(commands::CommandFailure{commands::ECommandError::STALE_TARGET});
                    const auto &value = *static_cast<const Rename *>(input.arguments().data());
                    auto changed = owner->get().edit(*target.based_on, value.bone, value.name, value.x);
                    if (!changed)
                        return cxx::unexpected(commands::CommandFailure{changed.error() == ESessionError::BUSY
                                                                            ? commands::ECommandError::BUSY
                                                                            : commands::ECommandError::STALE_CONTENT,
                                                                        "skeleton.edit",
                                                                        static_cast<std::uint64_t>(changed.error())});
                    return commands::DispatchReceipt{commands::ImmediateCompletion{}};
                }));
            draft.views.push_back(std::make_shared<views::ViewFactoryEntry>(
                code,
                views::ViewFactoryDescriptor{views::ViewTypeId{"example.skeleton.view"},
                                             "Skeleton",
                                             cxx::typeToken<views::ContentViewInput>(),
                                             1,
                                             {kind}},
                [code, access](const views::ViewFactoryInput &input) -> views::ViewFactoryResult<views::DetachedView>
                {
                    const auto &binding = *static_cast<const views::ContentViewInput *>(input.binding());
                    if (binding.content.primary)
                    {
                        auto key = access.key(*binding.content.primary);
                        if (!key)
                            return cxx::unexpected(
                                views::ViewFactoryFailure{views::EViewFactoryError::CONSTRUCT, "skeleton.binding"});
                    }
                    return views::DetachedView{code,
                                               std::make_unique<Window>(input, access, binding.content),
                                               nullptr,
                                               nullptr,
                                               nullptr,
                                               nullptr,
                                               +[](const ui::Pane &pane) noexcept
                                               { return static_cast<const Window &>(pane).content(); },
                                               +[](ui::Pane &pane, const views::ViewContent &value)
                                               { return static_cast<Window &>(pane).bind(value); }};
                }));
            return {};
        }
    } // namespace
} // namespace skeleton
void skeleton_probe(skeleton::Facts *facts) noexcept
{
    skeleton::facts = facts;
}
lux::editor::sessions::SessionResult<lux::rdesc::Skeleton> skeleton_read(lux::editor::sessions::SessionId id)
{
    using namespace skeleton;
    auto key = observed_store->key<Session>(id);
    if (!key)
        return lux::cxx::unexpected(key.error());
    auto owner = observed_store->access<Session>().read(*key);
    if (!owner)
        return lux::cxx::unexpected(owner.error());
    return owner->get().read();
}
lux::editor::sessions::SessionResult<lux::editor::sessions::SessionInfo> skeleton_describe(
    lux::editor::sessions::SessionId id)
{
    return skeleton::observed_store->describe(id);
}
lux::editor::sessions::SessionResult<void> skeleton_read_guard(lux::editor::sessions::SessionId id,
                                                               void (*callback)(void *),
                                                               void *context)
{
    using namespace skeleton;
    auto key = observed_store->key<Session>(id);
    if (!key)
        return lux::cxx::unexpected(key.error());
    auto owner = observed_store->access<Session>().read(*key);
    if (!owner)
        return lux::cxx::unexpected(owner.error());
    auto invoke = [&] { callback(context); };
    return owner->get().withRead(invoke);
}
extern "C" SKELETON_EXPORT const lux::editor::extensions::EditorExtensionExports *lux_editor_exports_v8() noexcept
{
    using namespace skeleton;
    static const EditorExtensionExports exports{sizeof(exports),
                                                kEditorExtensionVersion,
                                                kEditorExtensionAbi,
                                                {.sessions = 1},
                                                &contribute,
                                                {.commands = 1, .views = 1},
                                                {.sessions = true},
                                                &activate};
    return &exports;
}

extern "C" SKELETON_EXPORT const skeleton::ProbeApi *skeleton_probe_api() noexcept
{
    static const skeleton::ProbeApi api{&skeleton_probe, &skeleton_read, &skeleton_describe, &skeleton_read_guard};
    return &api;
}
