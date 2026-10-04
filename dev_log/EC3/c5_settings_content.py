from pathlib import Path
import json
s=Path('E:/SyncForder/CodeRepos/lux-engine-ec2')
def write(path,text):
    (s/path).write_text(text.lstrip('\n'), encoding='utf-8')
write('editor/workbench/project/tools/include/lux/engine/editor/project/SettingsContent.hpp',r'''
#pragma once
#include <lux/engine/editor/configuration/Settings.hpp>
#include <lux/engine/editor/scene/ConfigurationEditor.hpp>
#include <lux/engine/editor/workspace/WorkspaceChanges.hpp>
#include <lux/engine/ui/Element.hpp>

namespace lux::editor::project
{
    struct SettingsLocation final
    {
        workspace::WorkspaceStore& store;
        workspace::WorkspaceChanges& changes;
        settings::ESettingsScope scope;
        std::string relative;
    };
    struct AppliedSetting final
    {
        std::shared_ptr<const settings::SettingsEntry> entry;
        std::vector<std::byte> bytes;
    };
    // Specific page wiring, shared only by settings pages and their assembly owner. Applied values
    // are confirmed presentation facts; the setting receiver remains the actual functional owner.
    struct SettingsContentInput final
    {
        cxx::move_only_function<std::vector<settings::SettingsPage>()> pages;
        std::vector<SettingsLocation> locations;
        std::vector<AppliedSetting> applied;
    };
    enum class ESettingsAction : std::uint8_t { APPLY, SAVE, REVERT, DEFAULTS };
    class SettingsContent final : public lux::ui::Element
    {
    public:
        SettingsContent(lux::ui::Element&, lux::ui::ElementId, std::shared_ptr<SettingsContentInput>);
        ~SettingsContent() override;
        SettingsContent(const SettingsContent&) = delete;
        SettingsContent& operator=(const SettingsContent&) = delete;
        SettingsContent(SettingsContent&&) = delete;
        SettingsContent& operator=(SettingsContent&&) = delete;
        // Owner safe-point entry points also used by non-mouse accessibility/SDK consumers.
        [[nodiscard]] EditorResult<void> select(settings::SettingsIdView, settings::ESettingsScope);
        [[nodiscard]] EditorResult<void> request(ESettingsAction);
        [[nodiscard]] const settings::SettingsDraft* draft() const noexcept;
        [[nodiscard]] const std::optional<EditorFailure>& failure() const noexcept;
        void finishEdit(bool cancel = false) noexcept override;
    private:
        void draw() noexcept override;
        void update() noexcept override;
        void arrangeContent() noexcept override;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
''')
write('editor/workbench/project/tools/src/SettingsContent.cpp',r'''
#include <lux/engine/editor/project/SettingsContent.hpp>
#include <imgui.h>
#include <algorithm>

namespace lux::editor::project
{
    namespace
    {
        template<class Error> auto failure(std::string domain, const Error& error)
        {
            auto code = EEditorError::SOURCE_FAILURE;
            if constexpr (requires { error.code == decltype(error.code)::BUSY; })
                if (error.code == decltype(error.code)::BUSY)
                    code = EEditorError::BUSY;
            return cxx::unexpected(EditorFailure{code, std::move(domain), 0, {}, error});
        }
        EditorResult<settings::SettingsDocument> read(const SettingsLocation& location)
        {
            auto document = location.store.readSettings(location.relative, location.scope);
            if (document)
                return std::move(*document);
            if (document.error().code != workspace::EWorkspaceError::NOT_FOUND)
                return failure("settings.read", document.error());
            settings::SettingsDocument empty;
            empty.scope = location.scope;
            return empty;
        }
        void finish(lux::object::LuxObject& node, bool cancel) noexcept
        {
            for (auto* child = node.firstChild(); child; child = child->nextSibling())
                finish(*child, cancel);
            if (auto* element = dynamic_cast<lux::ui::Element*>(&node))
                element->finishEdit(cancel);
        }
    }
    struct SettingsContent::Impl final
    {
        struct Editing final
        {
            // Destruction order is part of the plugin contract: control, value, then factory code.
            settings::SettingsPage page;
            settings::SettingsDraft draft;
            std::unique_ptr<lux::ui::Element> control;
            struct Publication final
            {
                persistence::WriteTicket ticket;
                std::vector<std::byte> bytes;
            };
            std::optional<Publication> pending;
            Editing(settings::SettingsPage page, settings::SettingsDraft draft)
                : page(std::move(page)), draft(std::move(draft)) {}
        };
        std::shared_ptr<SettingsContentInput> input;
        std::vector<settings::SettingsPage> pages;
        std::unique_ptr<Editing> editing;
        std::optional<EditorFailure> error;
        std::optional<ESettingsAction> action;
        std::optional<std::pair<std::string, settings::ESettingsScope>> selection;
        float controls_y{};
        bool initialized{};
        explicit Impl(std::shared_ptr<SettingsContentInput> input) : input(std::move(input)) {}

        SettingsLocation* location(settings::ESettingsScope scope)
        {
            auto found = std::ranges::find(input->locations, scope, &SettingsLocation::scope);
            return found == input->locations.end() ? nullptr : &*found;
        }
        EditorResult<void> select(SettingsContent& owner, settings::SettingsIdView id, settings::ESettingsScope scope)
        {
            auto* target = location(scope);
            if (!target)
                return cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "settings.scope"});
            if (editing && editing->pending)
                return cxx::unexpected(EditorFailure{EEditorError::BUSY, "settings.publication.pending"});
            auto catalog = input->pages();
            const auto found = std::ranges::find_if(catalog, [&](const auto& page) {
                return page.entry->descriptor().id.name() == id.name();
            });
            if (found == catalog.end())
                return cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "settings.registration"});
            std::vector<settings::SettingsDocument> documents;
            std::optional<std::size_t> target_index;
            for (const auto& source : input->locations)
            {
                auto document = read(source);
                if (!document)
                    return cxx::unexpected(document.error());
                if (source.scope == scope)
                    target_index = documents.size();
                documents.push_back(std::move(*document));
            }
            auto resolved = settings::resolveSettings(found->entry, documents);
            if (!resolved)
                return failure("settings.resolve", resolved.error());
            auto draft = settings::makeSettingsDraft(*resolved, documents[*target_index]);
            if (!draft)
                return failure("settings.draft", draft.error());
            for (const auto& applied : input->applied)
                if (applied.entry == found->entry)
                    draft->applied = applied.bytes;
            auto candidate = std::make_unique<Editing>(*found, std::move(*draft));
            if (candidate->page.create)
            {
                auto control = candidate->page.create(owner, lux::ui::ElementId{"settings-fields"},
                    candidate->draft.desired);
                if (!control)
                    return cxx::unexpected(control.error());
                candidate->control = std::move(*control);
            }
            if (editing && editing->control)
                finish(*editing->control, false);
            editing = std::move(candidate);
            pages = std::move(catalog);
            return {};
        }
        EditorResult<void> request(SettingsContent& owner, ESettingsAction requested)
        {
            if (!editing)
                return cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "settings.no-draft"});
            if (editing->pending)
                return cxx::unexpected(EditorFailure{EEditorError::BUSY, "settings.publication.pending"});
            if (editing->control)
                finish(*editing->control, false);
            if (requested == ESettingsAction::REVERT)
                return select(owner, editing->page.entry->descriptor().id, editing->draft.scope);
            auto catalog = input->pages();
            const auto entry = std::ranges::find_if(catalog, [&](const auto& page) {
                return page.entry == editing->page.entry;
            });
            if (entry == catalog.end())
                return cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "settings.registration"});
            auto& draft = editing->draft;
            auto* target = location(draft.scope);
            auto source = read(*target);
            if (!source)
                return cxx::unexpected(source.error());
            if (requested == ESettingsAction::DEFAULTS)
            {
                auto defaults = entry->entry->defaults();
                if (!defaults)
                    return failure("settings.defaults", defaults.error());
                // The existing value object keeps its address for the generated control's accessor.
                draft.desired = std::move(*defaults);
                return {};
            }
            auto prepared = settings::prepareSettings(draft, *source, *entry->entry);
            if (!prepared)
                return failure("settings.prepare", prepared.error());
            std::vector<std::byte> bytes;
            auto encoded = draft.desired.encode(bytes);
            if (!encoded)
                return failure("settings.encode", encoded.error());
            const bool is_restart = entry->entry->descriptor().apply == settings::ESettingsApply::RESTART;
            if (!is_restart)
            {
                auto applied = entry->entry->apply(draft.desired);
                if (!applied)
                    return failure("settings.apply", applied.error());
                draft.applied = bytes;
                auto fact = std::ranges::find(input->applied, draft.entry, &AppliedSetting::entry);
                if (fact == input->applied.end())
                    input->applied.push_back({draft.entry, bytes});
                else
                    fact->bytes = bytes;
            }
            if (requested == ESettingsAction::SAVE)
            {
                auto accepted = target->changes.saveSettings(target->relative, *prepared);
                if (!accepted)
                    return cxx::unexpected(accepted.error());
                editing->pending = Editing::Publication{*accepted, std::move(bytes)};
            }
            else if (is_restart)
                return cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "settings.restart-required",
                    0, "Save the desired value and reopen the application to apply it."});
            return {};
        }
        void receive()
        {
            if (!editing || !editing->pending)
                return;
            auto& pending = *editing->pending;
            auto* target = location(editing->draft.scope);
            auto results = target->changes.publications();
            const auto found = std::ranges::find(results, pending.ticket, &workspace::WorkspacePublication::ticket);
            if (found == results.end() || !found->result)
                return;
            if (const auto* published = std::get_if<persistence::CommitReceipt>(&*found->result))
            {
                editing->draft.persisted = pending.bytes;
                editing->draft.based_on = published->version;
            }
            else
                error = EditorFailure{EEditorError::SOURCE_FAILURE, "settings.publish", 0,
                    "Publication failed. The desired value and last applied value are retained.", *found->result};
            // This page has consumed its result. Unknown never arrives as a terminal result here.
            auto acknowledged = target->changes.acknowledge(pending.ticket);
            if (!acknowledged)
            {
                error = acknowledged.error();
                return;
            }
            editing->pending.reset();
        }
    };
    SettingsContent::SettingsContent(lux::ui::Element& parent, lux::ui::ElementId id,
        std::shared_ptr<SettingsContentInput> input)
        : Element(parent, std::move(id)), impl_(std::make_unique<Impl>(std::move(input)))
    {
        setStretch({1, 1});
    }
    SettingsContent::~SettingsContent() = default;
    EditorResult<void> SettingsContent::select(settings::SettingsIdView id, settings::ESettingsScope scope)
    {
        auto result = impl_->select(*this, id, scope);
        impl_->error = result ? std::optional<EditorFailure>{} : result.error();
        return result;
    }
    EditorResult<void> SettingsContent::request(ESettingsAction action)
    {
        auto result = impl_->request(*this, action);
        impl_->error = result ? std::optional<EditorFailure>{} : result.error();
        return result;
    }
    const settings::SettingsDraft* SettingsContent::draft() const noexcept
    {
        return impl_->editing ? &impl_->editing->draft : nullptr;
    }
    const std::optional<EditorFailure>& SettingsContent::failure() const noexcept { return impl_->error; }
    void SettingsContent::finishEdit(bool cancel) noexcept
    {
        if (impl_->editing && impl_->editing->control)
            finish(*impl_->editing->control, cancel);
    }
    void SettingsContent::arrangeContent() noexcept
    {
        if (impl_->editing && impl_->editing->control)
            impl_->editing->control->arrange({{}, {rect().size.width, std::max(0.f, rect().size.height - 180.f)}});
    }
    void SettingsContent::update() noexcept
    {
        impl_->receive();
        if (!impl_->initialized)
        {
            impl_->pages = impl_->input->pages();
            impl_->initialized = true;
            if (!impl_->pages.empty() && !impl_->input->locations.empty())
                (void)select(impl_->pages.front().entry->descriptor().id, impl_->input->locations.back().scope);
        }
        if (auto selection = std::exchange(impl_->selection, {}))
            (void)select(settings::SettingsIdView{selection->first}, selection->second);
        if (auto action = std::exchange(impl_->action, {}))
            (void)request(*action);
    }
    void SettingsContent::draw() noexcept
    {
        auto& data = *impl_;
        const auto* selected = draft();
        const auto label = selected ? selected->entry->descriptor().label : std::string_view{"Choose settings"};
        ImGui::BeginDisabled(data.editing && data.editing->pending.has_value());
        if (ImGui::BeginCombo("Page", std::string{label}.c_str()))
        {
            for (const auto& page : data.pages)
                if (ImGui::Selectable(std::string{page.entry->descriptor().label}.c_str()))
                    data.selection = {std::string{page.entry->descriptor().id.name()},
                        selected ? selected->scope : data.input->locations.back().scope};
            ImGui::EndCombo();
        }
        if (selected)
        {
            int scope = selected->scope == settings::ESettingsScope::USER ? 0 : 1;
            if (ImGui::Combo("Scope", &scope, "User\0This project for this user\0"))
                data.selection = {std::string{selected->entry->descriptor().id.name()},
                    scope == 0 ? settings::ESettingsScope::USER : settings::ESettingsScope::USER_PROJECT};
            for (const auto& [text, action] : std::array{
                std::pair{"Apply", ESettingsAction::APPLY}, std::pair{"Save", ESettingsAction::SAVE},
                std::pair{"Revert", ESettingsAction::REVERT}, std::pair{"Defaults", ESettingsAction::DEFAULTS}
            })
            {
                if (ImGui::Button(text))
                    data.action = action;
                ImGui::SameLine();
            }
            ImGui::NewLine();
            const bool restart = selected->entry->descriptor().apply == settings::ESettingsApply::RESTART;
            ImGui::TextUnformatted(restart ? "Saved changes apply after restart." : "Changes apply at this UI safe point.");
            ImGui::Text("Applied: %s; persisted: %s", selected->applied ? "confirmed value" : "not confirmed",
                selected->persisted ? "confirmed value" : "no saved value");
        }
        if (data.error)
            ImGui::TextWrapped("%s: %s", data.error->domain.c_str(), data.error->message.c_str());
        if (data.editing && data.editing->control)
        {
            const float y = ImGui::GetCursorPosY() - rect().position.y;
            drawChild(*data.editing->control, {0, y});
        }
        ImGui::EndDisabled();
    }
}
''')
