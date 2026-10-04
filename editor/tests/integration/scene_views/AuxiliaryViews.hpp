void auxiliaryViews(Fixture& f)
{
    bool completed{};
    auto task = take(f.execution.submit(
        {"P10 cancellable timer", "P10"},
        [timer = f.execution.timer()](process::TaskReporter reporter) noexcept {
            reporter.setPhase("waiting for cancellation");
            return stdexec::upon_error(
                stdexec::then(
                    timer.after(std::chrono::hours(1)),
                    []() noexcept -> cxx::expected<void, process::ETimerError> { return {}; }
                ),
                [](process::ETimerError error) noexcept -> cxx::expected<void, process::ETimerError> {
                    return cxx::unexpected(error);
                }
            );
        },
        [&](process::TTaskResult<void, process::ETimerError>&& result) noexcept {
            assert(!result && result.error().isCancelled());
            completed = true;
        }
    ));
    tasks::TaskMonitor monitor(f.messages.dispatcherRef(), f.execution);
    auto detached = tasks::makeTaskView(f.messages.dispatcherRef(), ui::PaneId{"tasks"}, monitor);
    auto* view = static_cast<tasks::TaskView*>(detached.pane());
    const auto id = take(f.desktop->views().adopt(detached, views::ViewRestoreKey{"tasks"})).id;
    auto another = tasks::makeTaskView(f.messages.dispatcherRef(), ui::PaneId{"tasks-second"}, monitor);
    auto* second = static_cast<tasks::TaskView*>(another.pane());
    const auto second_id = take(f.desktop->views().adopt(another, views::ViewRestoreKey{"tasks-second"})).id;
    f.frame();
    assert(view->tasks().rows().data() == second->tasks().rows().data());
    const auto stable = monitor.snapshot();
    for (int i{}; i != 1000; ++i)
        assert(monitor.snapshot().get() == stable.get());
    assert(std::ranges::find(view->tasks().rows(), task.id(), &process::TaskInfo::id) != view->tasks().rows().end());
    view->tasks().requestCancel(task.id());
    assert(f.desktop->update(ui::FrameInfo{{1000, 650}, 1.F / 60.F}));
    assert(view->tasks().rejectedCancellations().empty());
    assert(f.desktop->views().close(id));
    assert(take(f.desktop->views().drain()).completed == 1);
    assert(!completed); // Task completion belongs to its application owner, not the Pane.
    assert(f.desktop->views().close(second_id));
    assert(take(f.desktop->views().drain()).completed == 1);
    f.wait([&] { return completed; });

    editor::project::ProjectCatalogModel catalog(f.messages.dispatcherRef(), 71);
    std::optional<editor::material::PreparedMaterialData> opened;
    std::size_t opens{};
    assert(catalog.replace(
        "Actual saved material",
        {{asset::AssetId{uuid("material")}, asset::AssetId{uuid("material")}, 123, "material.lux"}}
    ));
    auto project = editor::project::makeProjectView(f.messages.dispatcherRef(), ui::PaneId{"project"}, catalog);
    auto* browser = static_cast<editor::project::ProjectView*>(project.pane());
    auto connection = take(object::LuxObject::connect(
        browser,
        &editor::project::ProjectView::openRequested,
        [&](AssetReference reference) noexcept {
            const auto found = std::ranges::find(catalog.entries(), reference.asset, &AssetCatalogEntry::id);
            assert(found != catalog.entries().end());
            std::ifstream file(f.files / found->path, std::ios::binary | std::ios::ate);
            assert(file);
            const auto size = file.tellg();
            assert(size > 0);
            std::vector<std::byte> bytes(static_cast<std::size_t>(size));
            file.seekg(0);
            file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            assert(file);
            opened = take(editor::material::MaterialCodec::decode(bytes));
            assert(opened->source.id == reference.asset);
            ++opens;
        }
    ));
    const auto project_id = take(f.desktop->views().adopt(project, views::ViewRestoreKey{"project"})).id;
    const auto reference = catalog.reference(catalog.entries().front().id);
    assert(browser->catalog().assets().size() == 1 && browser->requestOpen(reference));
    assert(opens == 1 && opened->source.name == "P10 material");
    for (auto failure :
         {editor::project::EProjectQueryError::BUSY,
          editor::project::EProjectQueryError::PERMISSION,
          editor::project::EProjectQueryError::IO})
    {
        catalog.setFailure(failure);
        assert(!browser->refresh() && browser->catalog().assets().size() == 1);
        assert(!browser->requestOpen(reference) && opens == 1);
    }
    catalog.setFailure({});
    struct Picker final : ui::Pane
    {
        ui::Layout layout;
        editor::project::AssetPickerElement asset;
        Picker(object::ObjectDispatcherRef dispatcher, editor::project::ProjectCatalogModel* query)
            : Pane(dispatcher, ui::PaneId{"picker"}, ui::PaneTypeId{"test.picker"}, "Picker"),
              layout(*this, ui::ElementId{"layout"}, ui::ELayoutType::VERTICAL),
              asset(layout, ui::ElementId{"asset"}, query, 123)
        {
            setContent(layout);
        }
    };
    auto picker = std::make_unique<Picker>(f.messages.dispatcherRef(), &catalog);
    auto* control = &picker->asset;
    views::DetachedView picker_view{lux::object::CodeLease::builtin(), std::move(picker)};
    const auto picker_id = take(f.desktop->views().adopt(picker_view, views::ViewRestoreKey{"picker"})).id;
    assert(control->select(reference) && control->value() == reference.asset);
    auto invalid = reference;
    ++invalid.project_instance;
    assert(!control->select(invalid) && control->value() == reference.asset);
    assert(catalog.replace("Actual saved material", {catalog.entries().begin(), catalog.entries().end()}));
    assert(!control->select(reference) && !browser->requestOpen(reference));
    auto replacement = std::vector<AssetCatalogEntry>{catalog.entries().begin(), catalog.entries().end()};
    replacement.front().magic = 456;
    assert(catalog.replace("Actual saved material", std::move(replacement)));
    const auto current = catalog.reference(reference.asset);
    assert(!control->select(current) && control->value() == reference.asset);
    assert(browser->refresh() && browser->catalog().version() == take(catalog.version()));
    f.frame();
    assert(f.desktop->views().close(picker_id) && f.desktop->views().close(project_id));
    f.wait([&] { return !f.desktop->views().describe(project_id) && !f.desktop->views().describe(picker_id); });
    std::printf("P10 actual services: timer cancellation after view close; catalog BUSY/IO/permission preserves rows; "
                "saved Material decoded through asset-open request; picker revision/type/project checked.\n");
}
