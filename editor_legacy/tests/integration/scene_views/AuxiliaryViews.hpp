void auxiliaryViews(Fixture& f)
{
    bool completed{};
    auto task = take(f.execution.submit(
        {"P10 cancellable timer", "P10"},
        [timer = f.execution.timer()](process::TaskReporter reporter) noexcept
        {
            reporter.setPhase("waiting for cancellation");
            return stdexec::upon_error(
                stdexec::then(
                    timer.after(std::chrono::hours(1)),
                    []() noexcept -> cxx::expected<void, process::ETimerError> { return {}; }
                ),
                [](process::ETimerError error) noexcept -> cxx::expected<void, process::ETimerError>
                { return cxx::unexpected(error); }
            );
        },
        [&](process::TTaskResult<void, process::ETimerError>&& result) noexcept
        {
            assert(!result && result.error().isCancelled());
            completed = true;
        }
    ));
    tasks::TaskMonitor monitor(f.messages.dispatcherRef(), f.execution);
    services::ServiceRegistry services(f.messages.dispatcherRef());
    auto scope = take(services.createScope());
    assert(scope.provide(services::ServiceNameView{"lux.editor.tasks.monitor"}, monitor));
    desktop::UiRegistry windows(f.messages.dispatcherRef(), services);
    auto factories = take(desktop::UiCatalog::prepare(
        {desktop::UiEntry::bind<tasks::kTaskView>(object::CodeLease::builtin()),
         desktop::UiEntry::bind<editor::project::kProjectView>(object::CodeLease::builtin())}
    ));
    assert(windows.publish(factories));
    auto& root = f.desktop->root();
    auto task_factory = take(factories.find(views::ViewTypeIdView{"lux.editor.tasks"}));
    auto detached =
        take(windows.create(task_factory, scope, {f.messages.dispatcherRef(), ui::PaneId{"tasks"}, {}, {}}));
    auto* view = static_cast<tasks::TaskView*>(detached.get());
    auto another =
        take(windows.create(task_factory, scope, {f.messages.dispatcherRef(), ui::PaneId{"tasks-second"}, {}, {}}));
    auto* second = static_cast<tasks::TaskView*>(another.get());
    assert(root.addSubPane(std::move(detached)) && root.addSubPane(std::move(another)));
    const auto id = take(root.identify(*view));
    const auto second_id = take(root.identify(*second));
    f.frame();
    assert(view->tasks().rows().data() == second->tasks().rows().data());
    const auto stable = monitor.snapshot();
    for (int i{}; i != 1000; ++i)
    {
        assert(monitor.snapshot().get() == stable.get());
    }
    assert(std::ranges::find(view->tasks().rows(), task.id(), &process::TaskInfo::id) != view->tasks().rows().end());
    view->tasks().requestCancel(task.id());
    assert(f.desktop->update(ui::FrameInfo{{1000, 650}, 1.F / 60.F}));
    assert(view->tasks().rejectedCancellations().empty());
    auto close = take(windows.prepareClose(root, std::span(&id, 1)));
    assert(root.commit(close));
    assert(!root.findPane(id));
    assert(!completed); // Task completion belongs to its application owner, not the Pane.
    auto close_second = take(windows.prepareClose(root, std::span(&second_id, 1)));
    assert(root.commit(close_second));
    assert(!root.findPane(second_id));
    assert(f.messages.collectRetired() >= 2);
    f.wait([&] { return completed; });

    editor::project::ProjectCatalogModel catalog(f.messages.dispatcherRef(), 71);
    std::optional<editor::material::PreparedMaterialData> opened;
    std::size_t opens{};
    assert(catalog.replace(
        "Actual saved material",
        {{asset::AssetId{uuid("material")}, asset::AssetId{uuid("material")}, 123, "material.lux"}}
    ));
    assert(scope.provide(services::ServiceNameView{"lux.editor.project.catalog"}, catalog));
    auto project_factory = take(factories.find(views::ViewTypeIdView{"lux.editor.project"}));
    auto project =
        take(windows.create(project_factory, scope, {f.messages.dispatcherRef(), ui::PaneId{"project"}, {}, {}}));
    auto* browser = static_cast<editor::project::ProjectView*>(project.get());
    auto connection = take(object::LuxObject::connect(
        browser,
        &editor::project::ProjectView::openRequested,
        [&](AssetReference reference) noexcept
        {
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
    assert(root.addSubPane(std::move(project)));
    const auto project_id = take(root.identify(*browser));
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
              asset(dispatcher, ui::ElementId{"asset"}, query, 123)
        {
            assert(layout.addSubElement(asset));
            assert(setContent(layout));
        }
    };
    auto picker = std::make_unique<Picker>(f.messages.dispatcherRef(), &catalog);
    auto* control = &picker->asset;
    auto* picker_pane = picker.get();
    assert(root.addSubPane(std::move(picker)));
    const auto picker_id = take(root.identify(*picker_pane));
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
    const std::array<ui::Pane*, 2> closing{browser, picker_pane};
    auto removal = take(root.prepareDetach(closing));
    assert(root.commit(removal));
    assert(!root.findPane(project_id) && !root.findPane(picker_id));
    assert(f.messages.collectRetired() >= 2);
    assert(scope.release() && scope.drained());
    std::printf("P10 actual services: timer cancellation after view close; catalog BUSY/IO/permission preserves rows; "
                "saved Material decoded through asset-open request; picker revision/type/project checked.\n");
}
