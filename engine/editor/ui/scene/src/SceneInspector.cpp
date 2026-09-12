#include <lux/engine/editor/ui/scene/SceneWorkspace.hpp>
#include <lux/engine/editor/ui/scene/ComponentReadBinding.hpp>
#include <lux/engine/editor/ui/scene/ScenePropertyEditors.hpp>
#include <algorithm>
#include <lux/engine/editor/ui/scene/SceneEditFailureDisplay.hpp>
namespace lux::editor::ui
{
    struct SceneInspector::Impl final
    {
        sessions::SceneSession &session;
        std::vector<ComponentReadBinding> readers;
        std::optional<sessions::SceneEntityRef> displayed;
        bool drawing{};
        std::optional<sessions::PropertyGesture> gesture;
        std::optional<sessions::SceneFailure> failure;
        bool light{}, commit_requested{};
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
    SceneInspector::~SceneInspector() noexcept
    {
        cancelEdit();
        if (impl_->gesture)
            std::terminate();
    }
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
        const bool editable = impl_->session.access() == sessions::ESceneAccess::EDIT_CONTENT;
        frame.textMuted(editable ? "Scene editing | Unsaved author values" : "Live inspection | Content is read-only");
        const auto projection = impl_->session.projectionFailure();
        if (projection && *projection)
        {
            frame.textMuted("Content is retained; the scene preview is waiting for synchronization.");
            frame.textMuted(detail::propertyFailureText(**projection));
        }
        if (impl_->failure)
        {
            frame.textMuted(detail::propertyFailureText(*impl_->failure));
            if (impl_->gesture && frame.smallButton("Retry commit"))
                impl_->commit_requested = true;
            if (impl_->gesture && frame.smallButton("Cancel edit"))
                cancelEdit();
        }
        const auto current = impl_->session.selection().current;
        if (current != impl_->displayed)
            cancelEdit();
        impl_->displayed = current;
        if (!current)
        {
            frame.textMuted("Select an entity in the outline");
            return;
        }
        auto data = impl_->session.readEntity(*current);
        if (!data)
        {
            frame.textMuted("The selected entity is unavailable");
            return;
        }
        const auto priority = [](const auto &component) {
            return component.canonical_schema == "lux.ecs.Transform3D" ? 0 :
                   component.canonical_schema == "lux.ecs.Light3D" ? 1 : 2;
        };
        std::sort(data->components.begin(), data->components.end(), [&](const auto &a, const auto &b) {
            const auto first = priority(a), second = priority(b);
            return first != second ? first < second : a.canonical_schema < b.canonical_schema;
        });
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
            const bool transform = component.canonical_schema == "lux.ecs.Transform3D";
            const bool light = component.canonical_schema == "lux.ecs.Light3D";
            if (editable && component.editable && data->authored && (transform || light))
            {
                auto author = impl_->session.readAuthor(*data->authored, true);
                if (!author)
                {
                    impl_->failure = author.error();
                    continue;
                }
                const bool supported = transform ? bool(author->transform) : bool(author->light);
                if (!supported)
                {
                    if (reader != impl_->readers.end()) reader->draw(impl_->session, *current, frame);
                    continue;
                }
                const auto edit = transform ? detail::editTransform(frame, *author->transform) :
                                              detail::editLight(frame, *author->light);
                if (edit.changed && !impl_->gesture)
                {
                    const auto begun = transform ? impl_->session.beginTransformEdit(*data->authored) :
                                                   impl_->session.beginLightEdit(*data->authored);
                    if (!begun)
                    {
                        impl_->failure = begun.error();
                        continue;
                    }
                    impl_->gesture = *begun;
                    impl_->light = light;
                }
                if (edit.changed && impl_->gesture && impl_->light == light)
                {
                    const auto preview = transform
                        ? impl_->session.previewTransform(*impl_->gesture, *author->transform)
                        : impl_->session.previewLight(*impl_->gesture, *author->light);
                    if (!preview)
                    {
                        impl_->failure = preview.error();
                        continue;
                    }
                    impl_->failure.reset();
                }
                if (edit.cancelled) cancelEdit();
                else if (edit.committed && impl_->gesture && impl_->light == light)
                    impl_->commit_requested = true;
            }
            else if (reader != impl_->readers.end())
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
    void SceneInspector::cancelEdit() noexcept
    {
        if (!dispatcherRef().isCurrent() || !impl_->gesture)
            return;
        const auto result = impl_->light ? impl_->session.cancelLightEdit(*impl_->gesture) :
                                          impl_->session.cancelTransformEdit(*impl_->gesture);
        if (!result && result.error().code == sessions::ESceneError::BUSY)
            return;
        impl_->gesture.reset();
        impl_->commit_requested = false;
        impl_->failure.reset();
    }
    void SceneInspector::consumeInput(const lux::ui::UiInputSnapshot &input) noexcept
    {
        if (!dispatcherRef().isCurrent())
            return;
        if (!visible() || !focused() || !input.window_focused ||
            input.pressed[static_cast<std::size_t>(lux::ui::EKey::ESCAPE)])
        {
            cancelEdit();
            return;
        }
        if (impl_->gesture && impl_->commit_requested)
        {
            impl_->commit_requested = false;
            const auto committed = impl_->light ? impl_->session.commitLightEdit(*impl_->gesture) :
                                                  impl_->session.commitTransformEdit(*impl_->gesture);
            if (!committed)
                impl_->failure = committed.error();
            else
            {
                impl_->gesture.reset();
                impl_->failure.reset();
            }
        }
    }
} // namespace lux::editor::ui
