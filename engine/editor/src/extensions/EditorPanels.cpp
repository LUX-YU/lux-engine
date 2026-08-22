#include <lux/engine/editor/extensions/EditorPanels.hpp>

#include <algorithm>
#include <utility>

namespace lux::editor
{
    namespace
    {
        [[nodiscard]] bool sameId(
            const PanelId& lhs,
            PanelIdView rhs) noexcept
        {
            return lhs.view() == rhs;
        }

        [[nodiscard]] bool validSpec(const EditorPanelSpec& spec) noexcept
        {
            return spec.id.isValid() && spec.provider.isValid() &&
                isValidPanelIdName(spec.id.name()) &&
                lux::extensions::isCanonicalStableName(spec.provider.name());
        }
    }

    struct EditorPanels::Request final
    {
        EditorPanelSpec spec;
        std::unique_ptr<lux::ui::Panel> panel;
        lux::extensions::ModuleLease module;
    };

    struct EditorPanels::Impl final
    {
        struct Owned final
        {
            // Declared first and therefore destroyed last.
            lux::extensions::ModuleLease module;
            EditorPanelSpec spec;
            std::unique_ptr<lux::ui::Panel> panel;
            lux::ui::PanelRegistration registration;
        };

        explicit Impl(lux::ui::UISystem& ui_value) noexcept : ui(ui_value) {}

        [[nodiscard]] std::size_t find(PanelIdView id) const noexcept
        {
            const auto found = std::ranges::find_if(
                panels,
                [id](const auto& value) noexcept
                {
                    return sameId(value->spec.id, id);
                });
            return static_cast<std::size_t>(found - panels.begin());
        }

        lux::ui::UISystem& ui;
        std::vector<std::unique_ptr<Owned>> panels;
    };

    EditorPanels::EditorPanels(lux::ui::UISystem& ui)
        : impl_(std::make_unique<Impl>(ui))
    {}

    EditorPanels::~EditorPanels() = default;

    lux::cxx::expected<void, EEditorPanelInstallError> EditorPanels::add(
        EditorPanelSpec spec,
        std::unique_ptr<lux::ui::Panel> panel)
    {
        std::vector<Request> requests;
        requests.push_back(Request{
            std::move(spec),
            std::move(panel),
            {}});
        return addBatch(std::move(requests));
    }

    lux::cxx::expected<void, EEditorPanelInstallError>
    EditorPanels::addBatch(std::vector<Request> requests)
    {
        for (std::size_t index = 0u; index < requests.size(); ++index)
        {
            const auto& request = requests[index];
            if (!request.panel || !validSpec(request.spec))
            {
                return lux::cxx::unexpected(
                    EEditorPanelInstallError::INVALID_PANEL);
            }
            for (const auto& existing : impl_->panels)
            {
                if (sameId(existing->spec.id, request.spec.id.view()))
                {
                    return lux::cxx::unexpected(
                        EEditorPanelInstallError::DUPLICATE_PANEL);
                }
                if (panelIdCollision(
                        existing->spec.id.view(), request.spec.id.view()))
                {
                    return lux::cxx::unexpected(
                        EEditorPanelInstallError::ID_COLLISION);
                }
            }
            for (std::size_t other = 0u; other < index; ++other)
            {
                if (sameId(
                        requests[other].spec.id,
                        request.spec.id.view()))
                {
                    return lux::cxx::unexpected(
                        EEditorPanelInstallError::DUPLICATE_PANEL);
                }
                if (panelIdCollision(
                        requests[other].spec.id.view(),
                        request.spec.id.view()))
                {
                    return lux::cxx::unexpected(
                        EEditorPanelInstallError::ID_COLLISION);
                }
            }
        }

        std::vector<std::unique_ptr<Impl::Owned>> installed;
        installed.reserve(requests.size());
        for (auto& request : requests)
        {
            request.panel->setVisible(request.spec.default_visible);
            auto registration = impl_->ui.registerPanel(*request.panel);
            if (!registration)
            {
                return lux::cxx::unexpected(
                    EEditorPanelInstallError::UI_REGISTRATION_FAILED);
            }
            auto owned = std::make_unique<Impl::Owned>();
            owned->module = std::move(request.module);
            owned->spec = std::move(request.spec);
            owned->panel = std::move(request.panel);
            owned->registration = std::move(*registration);
            installed.push_back(std::move(owned));
        }
        impl_->panels.reserve(impl_->panels.size() + installed.size());
        for (auto& panel : installed)
            impl_->panels.push_back(std::move(panel));
        return {};
    }

    bool EditorPanels::setVisible(
        PanelIdView id,
        bool visible) noexcept
    {
        const auto index = impl_->find(id);
        if (index == impl_->panels.size())
            return false;
        impl_->panels[index]->panel->setVisible(visible);
        return true;
    }

    bool EditorPanels::remove(PanelIdView id) noexcept
    {
        const auto index = impl_->find(id);
        if (index == impl_->panels.size() ||
            !impl_->panels[index]->spec.supports_removal)
        {
            return false;
        }
        impl_->panels.erase(
            impl_->panels.begin() + static_cast<std::ptrdiff_t>(index));
        return true;
    }

    lux::ui::Panel* EditorPanels::find(PanelIdView id) noexcept
    {
        const auto index = impl_->find(id);
        return index == impl_->panels.size()
            ? nullptr
            : impl_->panels[index]->panel.get();
    }

    const lux::ui::Panel* EditorPanels::find(PanelIdView id) const noexcept
    {
        const auto index = impl_->find(id);
        return index == impl_->panels.size()
            ? nullptr
            : impl_->panels[index]->panel.get();
    }

    std::vector<EditorPanelSnapshot> EditorPanels::snapshot() const
    {
        std::vector<EditorPanelSnapshot> result;
        result.reserve(impl_->panels.size());
        for (const auto& value : impl_->panels)
        {
            result.push_back(EditorPanelSnapshot{
                value->spec.id,
                value->spec.display_name,
                value->spec.default_visible,
                value->panel->isVisible(),
                value->spec.provider});
        }
        return result;
    }

    std::size_t EditorPanels::close() noexcept
    {
        const auto removed = impl_->panels.size();
        impl_->panels.clear();
        return removed;
    }

    EditorPanelInstallContext::EditorPanelInstallContext(
        lux::extensions::ModuleLease module) noexcept
        : module_(std::move(module))
    {}

    lux::cxx::expected<void, EEditorPanelInstallError>
    EditorPanelInstallContext::add(
        EditorPanelSpec spec,
        std::unique_ptr<lux::ui::Panel> panel)
    {
        if (!module_)
        {
            return lux::cxx::unexpected(
                EEditorPanelInstallError::MODULE_UNAVAILABLE);
        }
        spec.provider = module_->id();
        if (!panel || !validSpec(spec))
        {
            return lux::cxx::unexpected(
                EEditorPanelInstallError::INVALID_PANEL);
        }
        for (const auto& request : requests_)
        {
            if (sameId(request.spec.id, spec.id.view()))
            {
                return lux::cxx::unexpected(
                    EEditorPanelInstallError::DUPLICATE_PANEL);
            }
            if (panelIdCollision(request.spec.id.view(), spec.id.view()))
            {
                return lux::cxx::unexpected(
                    EEditorPanelInstallError::ID_COLLISION);
            }
        }
        requests_.push_back(EditorPanels::Request{
            std::move(spec),
            std::move(panel),
            module_});
        return {};
    }

    EditorPanelInstallContext::~EditorPanelInstallContext() = default;

    lux::cxx::expected<void, EEditorPanelInstallError>
    EditorPanelInstallContext::commit(EditorPanels& panels) &&
    {
        if (!module_)
        {
            return lux::cxx::unexpected(
                EEditorPanelInstallError::MODULE_UNAVAILABLE);
        }
        return panels.addBatch(std::move(requests_));
    }
} // namespace lux::editor
