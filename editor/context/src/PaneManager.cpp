#include <lux/engine/editor/PaneManager.hpp>
#include <lux/engine/ui/Root.hpp>
#include <algorithm>
#include <unordered_set>
#include <uuid.h>
#include <random>

namespace lux::editor
{
    PaneManager::PaneManager(lux::ui::Root& root, EditorContext& context) noexcept : root_(root), context_(context) {}
    PaneManager::~PaneManager() = default;

    EditorResult<void> PaneManager::setRegistrations(std::vector<PaneRegistration> registrations)
    {
        if (frozen_)
            return lux::cxx::unexpected(EditorFailure{EEditorError::CLOSING, "panes.registration"});
        std::unordered_set<std::string_view> names;
        for (const auto& entry : registrations)
        {
            const bool is_invalid = !entry.valid();
            const bool is_duplicate = !is_invalid && !names.insert(entry.type.name()).second;
            if (is_invalid || is_duplicate)
                return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "panes.registration"});
        }
        for (const auto& entry : registrations_)
            if (entry.code_lifetime && std::ranges::find(retained_code_, entry.code_lifetime) == retained_code_.end())
                retained_code_.push_back(entry.code_lifetime);
        registrations_ = std::move(registrations);
        ++revision_;
        return {};
    }
    std::span<const PaneRegistration> PaneManager::registrations() const noexcept
    {
        return registrations_;
    }
    std::span<const std::unique_ptr<lux::ui::Pane>> PaneManager::panes() const noexcept
    {
        return panes_;
    }
    PaneRegistration::CreateResult PaneManager::create(lux::ui::PaneTypeIdView type) noexcept
    {
        if (frozen_)
            return lux::cxx::unexpected(EditorFailure{EEditorError::CLOSING, "panes.create"});
        const auto entry =
            std::ranges::find_if(registrations_, [&](const auto& value) { return value.type.view() == type; });
        if (entry == registrations_.end())
            return lux::cxx::unexpected(EditorFailure{EEditorError::MISSING_PROVIDER, "panes.create"});
        const auto created = entry->create(*this);
        if (!created)
            return created;
        auto& pane = created->get();
        const bool is_registered = std::ranges::any_of(panes_, [&](const auto& owned) { return owned.get() == &pane; });
        const bool is_wrong_type = is_registered && pane.type().view() != type;
        if (!is_registered || is_wrong_type)
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "panes.factory"});
        show(pane);
        return created;
    }
    PaneRegistration::CreateResult PaneManager::adopt(std::unique_ptr<lux::ui::Pane> pane) noexcept
    {
        if (frozen_)
            return lux::cxx::unexpected(EditorFailure{EEditorError::CLOSING, "panes.adopt"});
        const bool is_invalid = !pane || pane->parent() != &root_;
        const bool is_duplicate = !is_invalid && find(pane->id().view());
        if (is_invalid || is_duplicate)
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "panes.adopt"});
        auto& result = *pane;
        panes_.push_back(std::move(pane));
        ++revision_;
        return std::ref(result);
    }
    lux::ui::Pane* PaneManager::find(lux::ui::PaneIdView id) const noexcept
    {
        const auto found = std::ranges::find_if(panes_, [&](const auto& pane) { return pane->id().view() == id; });
        return found == panes_.end() ? nullptr : found->get();
    }
    lux::ui::Pane* PaneManager::findFirst(lux::ui::PaneTypeIdView type) const noexcept
    {
        const auto found = std::ranges::find_if(panes_, [&](const auto& pane) { return pane->type().view() == type; });
        return found == panes_.end() ? nullptr : found->get();
    }
    lux::ui::PaneId PaneManager::makeId()
    {
        std::random_device source;
        uuids::basic_uuid_random_generator<std::random_device> generate{source};
        return lux::ui::PaneId{"pane/" + uuids::to_string(generate())};
    }
    std::uint64_t PaneManager::revision() const noexcept
    {
        return revision_;
    }
    bool PaneManager::erase(lux::ui::PaneIdView id) noexcept
    {
        if (frozen_)
            return false;
        const bool removed = std::erase_if(panes_, [&](const auto& pane) { return pane->id().view() == id; }) != 0;
        if (removed)
            ++revision_;
        return removed;
    }
    void PaneManager::setFrozen(bool frozen) noexcept
    {
        frozen_ = frozen;
    }
    bool PaneManager::frozen() const noexcept
    {
        return frozen_;
    }
    void PaneManager::show(lux::ui::Pane& pane) noexcept
    {
        pane.setVisible(true);
        static_cast<void>(root_.requestFocus(pane));
    }
    lux::ui::Root& PaneManager::root() const noexcept
    {
        return root_;
    }
    EditorContext& PaneManager::context() const noexcept
    {
        return context_;
    }
}
