#include <array>
#include "Model.hpp"
#include "Probe.hpp"
#include "Settings.hpp"
#include <lux/engine/editor/extensions/EditorExtension.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/Controls.hpp>

namespace
{
    constexpr lux::editor::commands::CommandDescriptor command_example_skeleton_rename{
        lux::editor::commands::CommandIdView{"example.skeleton.rename"},
        "Edit skeleton",
        "Skeleton",
        {},
        lux::editor::commands::ECommandScope::SESSION,
        1,
        lux::cxx::typeToken<skeleton::Rename>()
    };
}
namespace skeleton
{


    using namespace sessions;
    using namespace extensions;
    namespace
    {
        Facts *facts{};
        DisplayOptions display_options;
        std::uint64_t display_revision{};
        void registerSettings(meta::ReflectionRegistry& registry, meta::qual_type_index_fix_list&)
        {
            auto type = std::make_unique<meta::RefClass>();
            type->name = "DisplayOptions";
            type->full_name = cxx::type_name<DisplayOptions>();
            type->hash = cxx::type_hash<DisplayOptions>();
            type->type = meta::ref_type_of_v<DisplayOptions>;
            type->type.ptr = type.get();
            type->construct = [](void* storage) { std::construct_at(static_cast<DisplayOptions*>(storage)); };
            type->destruct = [](void* storage) { std::destroy_at(static_cast<DisplayOptions*>(storage)); };
            registry.registerClass(std::move(type));
        }
        const ConfigurationDescriptor display_configuration{
            "example.skeleton.display", 1, serialization::makePortableValueCodec<DisplayOptions>(),
            +[](meta::ReflectionRegistry& registry) noexcept { return registry.findClass(cxx::type_name<DisplayOptions>()); }
        };
        constexpr settings::SettingsDescriptor display_descriptor{
            settings::SettingsIdView{"example.skeleton.display"}, "Skeleton display", &display_configuration,
            settings::kPersonalScopes, settings::ESettingsApply::SAFE_POINT,
            +[](const ConfigurationValue&) noexcept -> settings::SettingsResult<void> { return {}; }
        };
        class DisplayControl final : public ui::Element
        {
        public:
            DisplayControl(ui::Element& parent, ui::ElementId id, ConfigurationValue& value)
                : Element(parent, std::move(id)), value_(value),
                  indices_(*this, ui::ElementId{"indices"}, "Show bone indices",
                           static_cast<const DisplayOptions*>(value.data())->show_indices)
            {
                auto connected = object::LuxObject::connect(&indices_, &ui::CheckBox::edited,
                    [this](const ui::EditResult& result) noexcept {
                        if (result.changed)
                            static_cast<DisplayOptions*>(value_.data())->show_indices = indices_.value();
                    });
                if (!connected)
                    std::terminate();
                connection_ = std::move(*connected);
            }
        private:
            ui::SizeHint sizeHintContent() noexcept override { return indices_.sizeHint(); }
            ui::SizeHint measureContent(float width) noexcept override { return indices_.measure(width); }
            void arrangeContent() noexcept override { indices_.arrange({{}, rect().size}); }
            void draw() noexcept override { drawChild(indices_); }
            ConfigurationValue& value_;
            ui::CheckBox indices_;
            object::Connection connection_;
        };
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
                if (draft_ && shown_revision_ == display_revision)
                    return;
                auto value = owner->get().read();
                if (!value || value->bones.empty())
                    return;
                const auto stamp = owner->get().describe().current;
                std::string rows;
                for (std::size_t i{}; i != value->bones.size(); ++i)
                    rows += (display_options.show_indices ? std::to_string(i) + ": " : std::string{}) +
                            value->bones[i].name + " (parent " +
                            std::to_string(value->bones[i].parent_index) + ")\n";
                bones_.setText(std::move(rows));
                shown_revision_ = display_revision;
                if (facts)
                {
                    facts->rows_prepared += static_cast<unsigned>(value->bones.size());
                    facts->indices_displayed = display_options.show_indices;
                }
                // A presentation preference refresh must not replace an existing editing draft.
                if (draft_)
                    return;
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
            std::uint64_t shown_revision_{};
        };
        ContributionResult<void> contribute(ContributionDraft &draft, contracts::CodeLease code)
        {
            draft.sessions.push_back(factory(code));
            draft.reflection.push_back({code, &registerSettings});
            draft.settings.push_back({settings::SettingsEntry::bind<display_descriptor>(code,
                [](const ConfigurationValue& value) -> settings::SettingsResult<void> {
                    display_options = *static_cast<const DisplayOptions*>(value.data());
                    ++display_revision;
                    if (facts)
                    {
                        ++facts->settings_applied;
                        facts->indices_applied = display_options.show_indices;
                    }
                    return {};
                }),
                +[](ui::Element& parent, ui::ElementId id, ConfigurationValue& value) noexcept
                    -> EditorResult<std::unique_ptr<ui::Element>> {
                    return std::make_unique<DisplayControl>(parent, std::move(id), value);
                }});
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
            draft.commands.push_back(commands::CommandEntry::bind<command_example_skeleton_rename>(code,
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
            draft.views.push_back(views::ViewFactoryEntry::create(
                code,
                views::ViewFactoryDescriptor{views::ViewTypeIdView{"example.skeleton.view"},
                                             "Skeleton",
                                             cxx::typeToken<views::ContentViewInput>(),
                                             1,
                                             std::array{sessions::SessionKindIdView{kind.name}}},
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
extern "C" SKELETON_EXPORT const lux::editor::extensions::EditorExtensionExports *lux_editor_exports_v9() noexcept
{
    using namespace skeleton;
    static const EditorExtensionExports exports{sizeof(exports),
                                                kEditorExtensionVersion,
                                                kEditorExtensionAbi,
                                                {.sessions = 1, .reflection = 1, .settings = 1},
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
