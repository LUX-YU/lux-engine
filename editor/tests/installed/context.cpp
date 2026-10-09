#include "../api_contract.hpp"
#include "../support/InstalledProjectFixture.hpp"
#include <cassert>
#include <cstdio>
#include <lux/engine/EngineContext.hpp>
#include <lux/engine/editor/AppErrors.hpp>
#include <lux/engine/editor/ContextErrors.hpp>
#include <lux/engine/editor/EditorComposition.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/error/ErrorRegistry.hpp>
#include <lux/engine/object/ObjectRuntime.hpp>
#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>
#include <lux/engine/resource/asset/storage/pak/PakAssetProvider.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <thread>
#include <type_traits>

using namespace lux;
using namespace lux::editor;

namespace FixtureErrors
{
    inline constexpr lux::error::ErrorId EditorUnused = lux::error::errorId("lux.editor.unused");
    inline constexpr lux::error::ErrorId EditorFirstAttempt = lux::error::errorId("lux.editor.first_attempt");
} // namespace FixtureErrors

namespace
{
    ProjectManifest manifest(std::string name)
    {
        uuids::uuid_name_generator id{*uuids::uuid::from_string("08d1775a-340c-4082-9385-91d06aa8cecc")};
        return {1, id(name), std::move(name)};
    }

    struct Service final
    {
        std::vector<int>& events;
        int id;

        ~Service()
        {
            events.push_back(id);
        }
    };

    struct Other final
    {
        std::vector<int>& events;

        ~Other()
        {
            events.push_back(2);
        }
    };

    struct Recursive final
    {
    };

    struct Retry final
    {
    };

    struct Missing final
    {
    };

    struct Tools final
    {
        unsigned serial;
    };

    bool match(const world::WorldDescription&) noexcept
    {
        return true;
    }

    bool alsoMatch(const world::WorldDescription&) noexcept
    {
        return true;
    }

    bool noMatch(const world::WorldDescription&) noexcept
    {
        return false;
    }
} // namespace

int main(int argc, char** argv)
{
    assert(argc == 2);
    if (std::string_view(argv[1]) == "--registration-conflict")
    {
        auto& registry = error::ErrorRegistry::instance();
        assert(registry.registerType({"lux.editor.invalid_window_extent", "Conflicting schema"}));
        auto host = LuxEngine::create();
        assert(!host && host.error().type == error::errorId("lux.error.registration"));
        assert(host.error().args[0] == Errors::EditorInvalidWindowExtent);
        assert(registry.find(Errors::EditorInvalidWindowExtent)->message == "Conflicting schema");
        return 0;
    }
    const auto path = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(path);
    auto& runtime = object::ObjectRuntime::instance();
    assert(runtime.isCurrent());
    std::vector<int> destroyed;
    unsigned constructed{}, attempts{}, tools{};
    asset::AssetVfs extension_assets;
    auto assemble = [&](EditorComposition& context) noexcept -> FrameworkResult<void>
    {
        assert(context.bindExtension(extension_assets));
        const auto duplicate = context.bindExtension(extension_assets);
        assert(!duplicate && duplicate.error() == engine::EContextExtensionError::DUPLICATE_TYPE);
        assert(context.registerServiceFactory<Service>(
            [&](EditorContext&) noexcept -> FrameworkResult<std::unique_ptr<Service>>
            {
                ++constructed;
                return std::make_unique<Service>(destroyed, 1);
            }
        ));
        assert(!context.registerServiceFactory<Service>(
            [](EditorContext&) noexcept -> FrameworkResult<std::unique_ptr<Service>>
            { return cxx::unexpected(error::Error{FixtureErrors::EditorUnused, {}}); }
        ));
        assert(context.registerServiceFactory<Other>(
            [&](EditorContext& context) noexcept -> FrameworkResult<std::unique_ptr<Other>>
            {
                assert(context.service<Service>());
                return std::make_unique<Other>(destroyed);
            }
        ));
        assert(context.registerServiceFactory<Recursive>(
            [](EditorContext& context) noexcept -> FrameworkResult<std::unique_ptr<Recursive>>
            {
                auto nested = context.service<Recursive>();
                assert(!nested && nested.error().type == error::errorId("lux.editor.recursive_service_factory"));
                return cxx::unexpected(std::move(nested.error()));
            }
        ));
        assert(context.registerServiceFactory<Retry>(
            [&](EditorContext&) noexcept -> FrameworkResult<std::unique_ptr<Retry>>
            {
                if (++attempts == 1)
                {
                    return cxx::unexpected(error::Error{FixtureErrors::EditorFirstAttempt, {}});
                }
                return std::make_unique<Retry>();
            }
        ));
        assert(context.registerSceneTool<Tools>(
            &match,
            [&](EditorContext&, const world::WorldDescription&) noexcept -> FrameworkResult<std::unique_ptr<Tools>>
            { return std::make_unique<Tools>(++tools); }
        ));
        assert(constructed == 0);
        static_assert(!api_contract::PublicServiceLookup<EditorComposition>);
        return {};
    };

    auto inspect = [&](EditorContext& context) noexcept
    {
        assert(context.extensions().find<asset::AssetVfs>() == &extension_assets);
        assert(context.extensions().find<Service>() == nullptr && constructed == 0);
        assert(std::as_const(context).extensions().find<asset::AssetVfs>() == &extension_assets);
        static_assert(std::is_same_v<decltype(std::as_const(context).engine()), const engine::EngineContext&>);
        static_assert(std::is_same_v<decltype(std::as_const(context).assets()), const asset::AssetVfs&>);
        assert(error::ErrorRegistry::instance().find(Errors::EditorInvalidWindowExtent));
        std::thread worker(
            [&]
            {
                auto rejected = context.service<Service>();
                assert(!rejected && rejected.error().type == Errors::EditorProjectServicesRequireOwnerThread);
            }
        );
        worker.join();
        assert(!context.service<Missing>());
        assert(context.service<Other>());
        auto first = context.service<Service>();
        assert(first && &first->get() == &context.service<Service>()->get() && constructed == 1);
        assert(!context.service<Recursive>());
        assert(!context.service<Retry>() && attempts == 1);
        assert(context.service<Retry>() && attempts == 2);
        world::WorldDescription world;
        auto a = context.sceneTools().create<Tools>(context, world);
        auto b = context.sceneTools().create<Tools>(context, world);
        assert(a && b && a->get() != b->get() && (*a)->serial == 1 && (*b)->serial == 2);
        static_assert(!api_contract::PublicRegistration<EditorServices>);
    };
    fixture::withContext({"First", path}, manifest("First"), assemble, inspect);
    assert((destroyed == std::vector<int>{2, 1}));
    auto factory = [](EditorContext&,
                      const world::WorldDescription&) noexcept -> FrameworkResult<std::unique_ptr<Tools>>
    { return std::make_unique<Tools>(9); };
    auto ambiguous_assembly = [&](EditorComposition& composition) noexcept -> FrameworkResult<void>
    {
        assert(composition.registerSceneTool<Tools>(&match, factory));
        return composition.registerSceneTool<Tools>(&alsoMatch, factory);
    };
    fixture::withContext(
        {"Second", path},
        manifest("Second"),
        ambiguous_assembly,
        [](EditorContext& context) noexcept
        {
            auto result = context.sceneTools().create<Tools>(context, {});
            assert(!result && result.error().type == error::errorId("lux.editor.ambiguous_scene_tool_rules"));
        }
    );
    fixture::withContext(
        {"Empty", path},
        manifest("Empty"),
        [](EditorComposition&) noexcept -> FrameworkResult<void> { return {}; },
        [](EditorContext& context) noexcept
        {
            assert(!context.extensions().find<asset::AssetVfs>());
            auto result = context.sceneTools().create<Tools>(context, {});
            assert(!result && result.error().type == error::errorId("lux.editor.no_matching_scene_tool_rule"));
        }
    );

    // Both real Contexts coexist only during B's factory. Inspect isolation there,
    // before the normal safe point replaces A. No second Context constructor/API.
    const asset::AssetId id{*uuids::uuid::from_string("fedcba98-7654-3210-fedc-ba9876543210")};
    const auto low = cxx::SharedBytes<>::copyOf(std::as_bytes(std::span("low")));
    const auto high = cxx::SharedBytes<>::copyOf(std::as_bytes(std::span("higher")));
    assert(asset::writePakFile(path / "low.pak", {{id, 1, "item", {}, low}}));
    assert(asset::writePakFile(path / "high.pak", {{id, 1, "item", {}, high}}));
    auto lower = asset::PakAssetProvider::loadFromFile(path / "low.pak");
    auto higher = asset::PakAssetProvider::loadFromFile(path / "high.pak");
    assert(lower && higher);
    std::weak_ptr<asset::IAssetProvider> lifetime = *higher;
    assert(writeProjectManifestAtomic(path / "A.luxproj", manifest("A"), EProjectWrite::REPLACE));
    assert(writeProjectManifestAtomic(path / "B.luxproj", manifest("B"), EProjectWrite::REPLACE));
    LuxEngine* host{};
    bool isolated{};
    EditorConfig config{"VFS isolation", 200, 160};
    config.layout = {{"probe", "probe", "Probe"}};
    auto made = LuxEngine::create(
        std::move(config),
        [&](EditorComposition& composition) noexcept -> FrameworkResult<void>
        {
            return composition.registerUiFactory(
                "probe",
                [&](EditorContext& candidate,
                    const PaneDescription&) noexcept -> FrameworkResult<std::unique_ptr<ui::Pane>>
                {
                    if (candidate.project().name == "B")
                    {
                        auto& a = *host->project();
                        auto& b = candidate;
                        assert(a.project().name == "A");
                        auto a_mount = a.assets().mount({"/Game", *lower, 0});
                        auto b_mount = b.assets().mount({"/Game", *lower, 0});
                        assert(a_mount && b_mount);
                        auto mounted = a.assets().mount({"/Game", *higher, 1});
                        assert(mounted);
                        auto frozen = a.assets().view().capture();
                        higher->reset();
                        assert(!lifetime.expired());
                        assert(a.assets().resolve("/Game/item") == id);
                        assert(a.assets().resolve("/Missing/item").isNull());
                        assert(a.assets().open(id)->bytes.size() == high.size());
                        assert(b.assets().open(id)->bytes.size() == low.size());
                        assert(a.assets().open(id, 1).error() == asset::EAssetStorageError::LIMIT_EXCEEDED);
                        assert(a.assets().pathOf(id) == a.assets().view().pathOf(id));
                        std::vector<asset::ProviderEntry> direct, view;
                        a.assets().enumerate([&](const auto& value) { direct.push_back(value); });
                        a.assets().view().enumerate([&](const auto& value) { view.push_back(value); });
                        assert(direct.size() == 1 && view.size() == 1 && direct[0].vpath == view[0].vpath);
                        *mounted = {};
                        assert(a.assets().open(id)->bytes.size() == low.size());
                        assert(frozen.open(id)->bytes.size() == high.size());
                        assert(!lifetime.expired());
                        isolated = true;
                    }
                    return std::make_unique<ui::Pane>("Probe");
                }
            );
        }
    );
    assert(made);
    host = made->get();
    auto connection = object::LuxObject::connect(
        host,
        &LuxEngine::projectChanged,
        [&]() noexcept
        {
            if (!host->project())
            {
                return;
            }
            if (host->project()->project().name == "A")
            {
                assert(fixture::open(*host, path / "B.luxproj"));
            }
            else
            {
                assert(isolated && lifetime.expired());
                host->window().exit();
            }
        }
    );
    assert(connection && fixture::open(*host, path / "A.luxproj") && host->run());
    assert(isolated && lifetime.expired());
    std::puts("PASS installed public run/Events: complete Context factories, service lifetime and two-project VFS");
}
