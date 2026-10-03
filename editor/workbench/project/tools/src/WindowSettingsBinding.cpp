#include <lux/engine/editor/project/DesktopSettings.hpp>
#include <lux/engine/editor/project/SettingsContent.hpp>
#include <lux/engine/editor/workspace/WorkspaceChanges.hpp>
#include <lux/engine/window/LuxWindow.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>
#include <algorithm>
#include <utility>

namespace lux::editor::project
{
    namespace
    {
        bool samePlacement(const window::WindowPlacement& first, const window::WindowPlacement& second)
        {
            return first.normal == second.normal && first.mode == second.mode &&
                first.display.name == second.display.name && first.display.work_area == second.display.work_area;
        }
        template<class Error> EditorFailure failed(std::string domain, const Error& cause)
        {
            return {EEditorError::SOURCE_FAILURE, std::move(domain), 0, {}, cause};
        }
    }
    struct WindowSettingsBinding::Impl final
    {
        std::shared_ptr<const settings::SettingsEntry> entry_;
        window::LuxWindow& window_;
        workspace::WorkspaceStore& store_;
        workspace::WorkspaceChanges& changes_;
        SettingsContentInput& input_;
        window::WindowPlacement observed_;
        std::optional<window::WindowPlacement> pending_;
        std::optional<std::pair<persistence::WriteTicket, window::WindowPlacement>> submitted_;
        std::optional<EditorFailure> failure_;
        std::uint64_t displays_{window::GlfwRuntime::displayRevision()};
        bool changed_{};

        Impl(window::LuxWindow& window, workspace::WorkspaceStore& store, workspace::WorkspaceChanges& changes,
            std::shared_ptr<const settings::SettingsEntry> entry, SettingsContentInput& input, window::WindowPlacement state)
            : entry_(std::move(entry)), window_(window), store_(store), changes_(changes), input_(input),
              observed_(std::move(state))
        {}
        ~Impl() { window_.on_placement_changed = {}; }

        EditorResult<void> observe()
        {
            const auto revision = window::GlfwRuntime::displayRevision();
            if (revision != displays_)
            {
                auto displays = window::LuxWindow::displays();
                if (!displays)
                    return cxx::unexpected(failed("window.displays", displays.error()));
                auto current = window_.state();
                if (!current)
                    return cxx::unexpected(failed("window.observe", current.error()));
                window::WindowPlacementRequest request;
                request.saved = current->placement;
                auto resolved = window::resolveWindowPlacement(request, *displays, current->insets);
                if (!resolved)
                    return cxx::unexpected(failed("window.resolve", resolved.error()));
                if (resolved->adjusted)
                {
                    auto applied = window_.applyPlacement(resolved->placement);
                    if (!applied)
                        return cxx::unexpected(failed("window.apply", applied.error()));
                }
                displays_ = revision;
                changed_ = true;
            }
            if (!std::exchange(changed_, false))
                return {};
            auto current = window_.state();
            if (!current)
                return cxx::unexpected(failed("window.observe", current.error()));
            // Never record minimized geometry or write the initial launch override back to disk.
            if (current->minimized || samePlacement(observed_, current->placement))
                return {};
            auto value = entry_->defaults();
            if (!value)
                return cxx::unexpected(failed("window.settings.defaults", value.error()));
            *static_cast<WindowSettings*>(value->data()) = {true, current->placement};
            std::vector<std::byte> bytes;
            if (auto encoded = value->encode(bytes); !encoded)
                return cxx::unexpected(failed("window.settings.encode", encoded.error()));
            auto applied = std::ranges::find(input_.applied, entry_, &AppliedSetting::entry);
            if (applied == input_.applied.end())
                input_.applied.push_back({entry_, std::move(bytes)});
            else
                applied->bytes = std::move(bytes);
            observed_ = std::move(current->placement);
            pending_ = observed_;
            failure_.reset();
            return {};
        }

        void update(bool allow_new_work)
        {
            if (auto observed = observe(); !observed)
                failure_ = observed.error();
            if (submitted_)
            {
                const auto reports = changes_.publications();
                const auto found = std::ranges::find(reports, submitted_->first, &workspace::WorkspacePublication::ticket);
                if (found == reports.end())
                {
                    failure_ = EditorFailure{EEditorError::STALE_REQUEST, "window.settings.receipt"};
                    return;
                }
                if (!found->result)
                    return; // Unknown still belongs to the original coordinator lane.
                if (!std::holds_alternative<persistence::CommitReceipt>(*found->result))
                {
                    failure_ = failed("window.settings.publication", *found->result);
                    if (!pending_)
                        pending_ = submitted_->second;
                }
                if (auto acknowledged = changes_.acknowledge(submitted_->first); !acknowledged)
                {
                    failure_ = acknowledged.error();
                    return;
                }
                submitted_.reset();
            }
            const bool can_publish = allow_new_work && pending_ && !failure_ && changes_.hasCapacity() &&
                !changes_.migrationPending();
            if (!can_publish)
                return;
            // A native event starts a fresh intent. Existing UI drafts keep their own older source
            // versions and will conflict; this does not rebase a user's uncommitted SettingsDraft.
            auto stored = store_.readSettings("settings.toml", settings::ESettingsScope::USER_PROJECT);
            if (!stored && stored.error().code != workspace::EWorkspaceError::NOT_FOUND)
            {
                failure_ = failed("window.settings.read", stored.error());
                return;
            }
            auto document = stored ? std::move(*stored) : settings::SettingsDocument{};
            document.scope = settings::ESettingsScope::USER_PROJECT;
            auto effective = settings::resolveSettings(entry_, std::span{&document, 1});
            if (!effective)
            {
                failure_ = failed("window.settings.resolve", effective.error());
                return;
            }
            auto draft = settings::makeSettingsDraft(*effective, document);
            if (!draft)
            {
                failure_ = failed("window.settings.draft", draft.error());
                return;
            }
            *static_cast<WindowSettings*>(draft->desired.data()) = {true, *pending_};
            auto prepared = settings::prepareSettings(*draft, document, *entry_);
            if (!prepared)
            {
                failure_ = failed("window.settings.prepare", prepared.error());
                return;
            }
            auto accepted = changes_.saveSettings("settings.toml", *prepared);
            if (!accepted)
            {
                if (accepted.error().code != EEditorError::BUSY)
                    failure_ = accepted.error();
                return;
            }
            submitted_ = std::pair{*accepted, std::move(*pending_)};
            pending_.reset();
        }
    };
    WindowSettingsBinding::WindowSettingsBinding(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
    WindowSettingsBinding::~WindowSettingsBinding() = default;
    EditorResult<std::unique_ptr<WindowSettingsBinding>> WindowSettingsBinding::create(
        window::LuxWindow& window, workspace::WorkspaceStore& store, workspace::WorkspaceChanges& changes,
        std::shared_ptr<const settings::SettingsEntry> entry, SettingsContentInput& input)
    {
        const bool has_configuration = entry && entry->descriptor().configuration;
        const bool is_invalid_type = !has_configuration ||
            entry->descriptor().configuration->codec.type != cxx::typeToken<WindowSettings>();
        if (is_invalid_type)
            return cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "window.settings.type"});
        if (window.on_placement_changed)
            return cxx::unexpected(EditorFailure{EEditorError::BUSY, "window.settings.connection"});
        auto state = window.state();
        if (!state)
            return cxx::unexpected(failed("window.observe", state.error()));
        auto impl = std::make_unique<Impl>(window, store, changes, std::move(entry), input, std::move(state->placement));
        auto result = std::unique_ptr<WindowSettingsBinding>(new WindowSettingsBinding(std::move(impl)));
        window.on_placement_changed = [owner = result->impl_.get()](const window::WindowPlacementEvent&) {
            owner->changed_ = true;
        };
        return result;
    }
    void WindowSettingsBinding::update(bool allow_new_work) { impl_->update(allow_new_work); }
    void WindowSettingsBinding::retry() noexcept
    {
        impl_->failure_.reset();
        impl_->changed_ = true;
    }
    const EditorFailure* WindowSettingsBinding::failure() const noexcept
    {
        return impl_->failure_ ? &*impl_->failure_ : nullptr;
    }
}
