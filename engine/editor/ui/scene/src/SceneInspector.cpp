#include <lux/engine/editor/ui/scene/SceneWorkspace.hpp>
#include <lux/engine/editor/ui/scene/ComponentReadBinding.hpp>
#include <algorithm>
namespace lux::editor::ui
{
    struct SceneInspector::Impl final
    {
        sessions::SceneSession &session;
        std::vector<ComponentReadBinding> readers;
        std::optional<sessions::SceneEntityRef> displayed;
        bool drawing{};
        explicit Impl(sessions::SceneSession &source) : session(source), readers(firstPartySceneReaders())
        {
        }
    };
    SceneInspector::SceneInspector(lux::object::ObjectDispatcherRef dispatcher, lux::ui::PaneId id,
                                   sessions::SceneSession &session)
        : Object(std::move(dispatcher), std::move(id), lux::ui::PaneTypeId{"lux.scene.inspector"}, "Inspector"),
          impl_(std::make_unique<Impl>(session))
    {
    }
    SceneInspector::~SceneInspector() noexcept = default;
    WindowResult<void> SceneInspector::installReaders(std::span<const ComponentReadBinding> readers) noexcept
    {
        if (!dispatcherRef().isCurrent())
            return lux::cxx::unexpected(WindowFailure{EWindowError::WRONG_THREAD});
        if (impl_->drawing)
            return lux::cxx::unexpected(WindowFailure{EWindowError::BUSY});
        for (std::size_t i = 0; i < readers.size(); ++i)
        {
            const bool has_identity = readers[i].version && !readers[i].canonical_schema.empty() &&
                                      readers[i].type.hash() && !readers[i].type.name().empty();
            if (!readers[i].draw || !has_identity)
                return lux::cxx::unexpected(WindowFailure{EWindowError::INVALID_ARGUMENT});
            for (std::size_t j = 0; j < i; ++j)
                if (readers[i].canonical_schema == readers[j].canonical_schema || readers[i].type == readers[j].type)
                    return lux::cxx::unexpected(WindowFailure{EWindowError::INVALID_ARGUMENT});
        }
        try
        {
            std::vector<ComponentReadBinding> prepared(readers.begin(), readers.end());
            impl_->readers.swap(prepared);
            return {};
        }
        catch (const std::bad_alloc &)
        {
            return lux::cxx::unexpected(WindowFailure{EWindowError::ALLOCATION_FAILURE});
        }
    }
    void SceneInspector::draw(lux::ui::Frame &frame, lux::ui::PaneDrawContext &context)
    {
        context.activateContext(lux::ui::UiContextIdView{id().name()});
        struct Guard final
        {
            bool &value;
            ~Guard() noexcept
            {
                value = false;
            }
        } guard{impl_->drawing};
        impl_->drawing = true;
        frame.textMuted("Live inspection | Content is read-only");
        const auto current = impl_->session.selection().current;
        impl_->displayed = current;
        if (!current)
        {
            frame.textMuted("Select an entity in the outline");
            return;
        }
        const auto data = impl_->session.readEntity(*current);
        if (!data)
        {
            frame.textMuted("The selected entity is unavailable");
            return;
        }
        for (const auto &component : data->components)
        {
            const auto reader = std::find_if(
                impl_->readers.begin(), impl_->readers.end(), [&](const auto &value)
                { return value.canonical_schema == component.canonical_schema && value.version == component.version; });
            const auto title = reader == impl_->readers.end() ? component.canonical_schema : reader->display_name;
            auto tree = frame.treeRow({lux::ui::WidgetIdView{component.canonical_schema}, title, false, false, true});
            if (!tree.open())
                continue;
            auto table = frame.table({lux::ui::WidgetIdView{component.canonical_schema}, 2, false, false, false, 140});
            if (!table.visible())
                continue;
            if (reader != impl_->readers.end())
                reader->draw(impl_->session, *current, frame);
            else
            {
                frame.propertyRow("Schema");
                frame.text(component.canonical_schema + " v" + std::to_string(component.version));
                frame.propertyRow("Fields");
                frame.textMuted("No field UI is registered for this component");
            }
        }
    }
} // namespace lux::editor::ui
