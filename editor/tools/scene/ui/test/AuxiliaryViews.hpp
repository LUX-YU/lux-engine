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
    auto detached =
        tasks::makeTaskView(f.messages.dispatcherRef(), ui::PaneId{"tasks"}, tasks::TaskQueryPort{f.execution});
    auto* view = static_cast<tasks::TaskView*>(detached.pane());
    const auto id = take(f.desktop->views().adopt(detached, views::ViewRestoreKey{"tasks"})).id;
    f.frame();
    assert(std::ranges::find(view->tasks().rows(), task.id(), &process::TaskInfo::id) != view->tasks().rows().end());
    view->tasks().requestCancel(task.id());
    assert(f.desktop->update(ui::FrameInfo{{1000, 650}, 1.F / 60.F}));
    assert(view->tasks().rejectedCancellations().empty());
    assert(f.desktop->views().close(id));
    assert(take(f.desktop->views().drain()).completed == 1);
    assert(!completed); // Task completion belongs to its application owner, not the Pane.
    f.wait([&] { return completed; });

    struct Catalog final
    {
        editor::project::ProjectCatalog value;
        std::filesystem::path root;
        std::optional<editor::project::EProjectQueryError> failure;
        std::optional<editor::material::PreparedMaterialData> opened;
        std::size_t opens{};
    } catalog{
        {{71, 1},
         "Actual saved material",
         {{asset::AssetId{uuid("material")}, asset::AssetId{uuid("material")}, 123, "material.lux"}}},
        f.files
    };
    const editor::project::ProjectCatalogAccess query{
        &catalog,
        [](const void* owner) -> editor::project::ProjectQueryResult<editor::project::ProjectCatalogVersion> {
            const auto& state = *static_cast<const Catalog*>(owner);
            if (state.failure)
                return cxx::unexpected(editor::project::VProjectQueryFailure{*state.failure});
            return state.value.version;
        },
        [](const void* owner) -> editor::project::ProjectQueryResult<editor::project::ProjectCatalog> {
            const auto& state = *static_cast<const Catalog*>(owner);
            if (state.failure)
                return cxx::unexpected(editor::project::VProjectQueryFailure{*state.failure});
            return state.value;
        },
        [](const void* owner, AssetReference reference, std::uint32_t magic
        ) -> editor::project::ProjectQueryResult<asset::AssetId> {
            const auto& state = *static_cast<const Catalog*>(owner);
            if (state.failure)
                return cxx::unexpected(editor::project::VProjectQueryFailure{*state.failure});
            if (reference.project_instance != state.value.version.instance)
                return cxx::unexpected(editor::project::VProjectQueryFailure{EAssetReferenceError::FOREIGN_PROJECT});
            if (reference.catalog_revision != state.value.version.revision)
                return cxx::unexpected(editor::project::VProjectQueryFailure{EAssetReferenceError::STALE_CATALOG});
            const auto found = std::ranges::find(state.value.assets, reference.asset, &AssetCatalogEntry::id);
            if (found == state.value.assets.end())
                return cxx::unexpected(editor::project::VProjectQueryFailure{EAssetReferenceError::MISSING_ASSET});
            if (magic && magic != found->magic)
                return cxx::unexpected(editor::project::VProjectQueryFailure{EAssetReferenceError::WRONG_TYPE});
            return found->id;
        }
    };
    const editor::project::AssetOpenRequests requests{
        &catalog,
        [](void* owner, AssetReference reference) -> editor::project::ProjectQueryResult<void> {
            auto& state = *static_cast<Catalog*>(owner);
            const auto found = std::ranges::find(state.value.assets, reference.asset, &AssetCatalogEntry::id);
            assert(found != state.value.assets.end());
            std::ifstream file(state.root / found->path, std::ios::binary | std::ios::ate);
            if (!file)
                return cxx::unexpected(editor::project::VProjectQueryFailure{editor::project::EProjectQueryError::IO});
            const auto size = file.tellg();
            assert(size > 0);
            std::vector<std::byte> bytes(static_cast<std::size_t>(size));
            file.seekg(0);
            file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            assert(file);
            state.opened = take(editor::material::MaterialCodec::decode(bytes));
            assert(state.opened->source.id == reference.asset);
            ++state.opens;
            return {};
        }
    };
    auto project = editor::project::makeProjectView(f.messages.dispatcherRef(), ui::PaneId{"project"}, query, requests);
    auto* browser = static_cast<editor::project::ProjectView*>(project.pane());
    const auto project_id = take(f.desktop->views().adopt(project, views::ViewRestoreKey{"project"})).id;
    const auto reference = catalog.value.reference(catalog.value.assets.front().id);
    assert(browser->catalog().assets.size() == 1 && browser->requestOpen(reference));
    assert(catalog.opens == 1 && catalog.opened->source.name == "P10 material");
    for (auto failure :
         {editor::project::EProjectQueryError::BUSY,
          editor::project::EProjectQueryError::PERMISSION,
          editor::project::EProjectQueryError::IO})
    {
        catalog.failure = failure;
        assert(!browser->refresh() && browser->catalog().assets.size() == 1);
        assert(!browser->requestOpen(reference) && catalog.opens == 1);
    }
    catalog.failure.reset();
    struct Picker final : ui::Pane
    {
        ui::Layout layout;
        editor::project::AssetPickerElement asset;
        Picker(object::ObjectDispatcherRef dispatcher, editor::project::ProjectCatalogAccess query)
            : Pane(dispatcher, ui::PaneId{"picker"}, ui::PaneTypeId{"test.picker"}, "Picker"),
              layout(*this, ui::ElementId{"layout"}, ui::ELayoutType::VERTICAL),
              asset(layout, ui::ElementId{"asset"}, query, 123)
        {
            setContent(layout);
        }
    };
    auto picker = std::make_unique<Picker>(f.messages.dispatcherRef(), query);
    auto* control = &picker->asset;
    views::DetachedView picker_view{contracts::CodeLease::builtin(), std::move(picker)};
    const auto picker_id = take(f.desktop->views().adopt(picker_view, views::ViewRestoreKey{"picker"})).id;
    assert(control->select(reference) && control->value() == reference.asset);
    auto invalid = reference;
    ++invalid.project_instance;
    assert(!control->select(invalid) && control->value() == reference.asset);
    ++catalog.value.version.revision;
    assert(!control->select(reference) && !browser->requestOpen(reference));
    const auto current = catalog.value.reference(reference.asset);
    catalog.value.assets.front().magic = 456;
    assert(!control->select(current) && control->value() == reference.asset);
    assert(browser->refresh() && browser->catalog().version == catalog.value.version);
    f.frame();
    assert(f.desktop->views().close(picker_id) && f.desktop->views().close(project_id));
    f.wait([&] { return !f.desktop->views().describe(project_id) && !f.desktop->views().describe(picker_id); });
    std::printf("P10 actual services: timer cancellation after view close; catalog BUSY/IO/permission preserves rows; "
                "saved Material decoded through asset-open request; picker revision/type/project checked.\n");
}
