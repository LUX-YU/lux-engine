#include "api_contract.hpp"
#include "support/ContextFixture.hpp"
#include <cassert>
#include <cstdio>
#include <lux/engine/EngineContext.hpp>
#include <lux/engine/editor/EditorComposition.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/error/ErrorRegistry.hpp>
#include <lux/engine/object/ObjectRuntime.hpp>
#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>
#include <lux/engine/resource/asset/storage/pak/PakAssetProvider.hpp>
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
    if (argc == 2 && std::string_view(argv[1]) == "--registration-conflict")
    {
        auto& registry = error::ErrorRegistry::instance();
        assert(registry.registerType({"lux.editor.invalid_window_extent", "Conflicting schema"}));
        auto& runtime = object::ObjectRuntime::instance();
        auto engine = engine::EngineContext::create({1, 64, 64, {32}}, {0, 64});
        assert(engine && runtime.isCurrent());
        bool assembled{};
        auto assembly = [&](EditorComposition&) noexcept -> FrameworkResult<void>
        {
            assembled = true;
            return {};
        };
        auto context = fixture::createContext(
            **engine,
            {"Conflict", std::filesystem::current_path()},
            manifest("Conflict"),
            assembly
        );
        assert(!context && !assembled);
        assert(context.error().type == error::errorId("lux.error.registration"));
        assert(context.error().args[0] == Errors::EditorInvalidWindowExtent);
        assert(registry.find(Errors::EditorInvalidWindowExtent)->message == "Conflicting schema");
        return 0;
    }

    const lux::error::ErrorDescriptor fixture_errors[]{
        {"lux.editor.unused", "unused", lux::error::ERecovery::PERMANENT},
        {"lux.editor.first_attempt", "first attempt", lux::error::ERecovery::PERMANENT}
    };
    assert(lux::error::ErrorRegistry::instance().registerTypes(fixture_errors));

    static_assert(!std::is_move_constructible_v<EditorContext>);
    static_assert(!std::is_copy_constructible_v<EditorServices>);
    static_assert(!std::is_move_constructible_v<EditorUiRegistry>);
    static_assert(!std::is_copy_constructible_v<SceneToolRegistry>);
    auto& queue = object::ObjectRuntime::instance();
    auto engine = engine::EngineContext::create({1, 64, 64, {32}}, {0, 64});
    assert(queue.isCurrent() && engine);
    std::vector<int> destroyed;
    unsigned constructed{}, attempts{}, tools{};
    {
        auto assemble = [&](EditorComposition& context) noexcept -> FrameworkResult<void>
        {
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
        auto created =
            fixture::createContext(**engine, {"First", std::filesystem::current_path()}, manifest("First"), assemble);
        assert(created);
        auto& context = **created;
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
        auto factory = [](EditorContext&,
                          const world::WorldDescription&) noexcept -> FrameworkResult<std::unique_ptr<Tools>>
        { return std::make_unique<Tools>(9); };
        auto assemble_second = [&](EditorComposition& second) noexcept -> FrameworkResult<void>
        {
            assert(second.registerSceneTool<Tools>(&match, factory));
            assert(second.registerSceneTool<Tools>(&alsoMatch, factory));
            return {};
        };
        auto second_owner = fixture::createContext(
            **engine,
            {"Second", std::filesystem::current_path()},
            manifest("Second"),
            assemble_second
        );
        assert(second_owner);
        auto& second = **second_owner;
        auto ambiguous = second.sceneTools().create<Tools>(second, world);
        assert(!ambiguous && ambiguous.error().type == error::errorId("lux.editor.ambiguous_scene_tool_rules"));
        auto empty_assembly = [](EditorComposition&) noexcept -> FrameworkResult<void> { return {}; };
        auto empty = fixture::createContext(
            **engine,
            {"Empty", std::filesystem::current_path()},
            manifest("Empty"),
            empty_assembly
        );
        assert(empty);
        auto missing = (*empty)->sceneTools().create<Tools>(**empty, world);
        assert(!missing && missing.error().type == error::errorId("lux.editor.no_matching_scene_tool_rule"));
    }
    assert((destroyed == std::vector<int>{2, 1}));
    std::puts("PASS lazy services, build-only registration, recursion, retry, reverse destruction, typed tool rules");
    assert(argc == 2);
    const auto path = std::filesystem::path(argv[1]) / "framework-vfs";
    std::filesystem::create_directories(path);
    const asset::AssetId id{*uuids::uuid::from_string("fedcba98-7654-3210-fedc-ba9876543210")};
    const auto low = cxx::SharedBytes<>::copyOf(std::as_bytes(std::span("low")));
    const auto high = cxx::SharedBytes<>::copyOf(std::as_bytes(std::span("higher")));
    assert(asset::writePakFile(path / "low.pak", {{id, 1, "item", {}, low}}));
    assert(asset::writePakFile(path / "high.pak", {{id, 1, "item", {}, high}}));
    auto lower = asset::PakAssetProvider::loadFromFile(path / "low.pak");
    auto higher = asset::PakAssetProvider::loadFromFile(path / "high.pak");
    assert(lower && higher);
    std::weak_ptr<asset::IAssetProvider> lifetime = *higher;
    {
        auto empty_assembly = [](EditorComposition&) noexcept -> FrameworkResult<void> { return {}; };
        auto first_context = fixture::createContext(**engine, {"A", path}, manifest("A"), empty_assembly);
        auto second_context = fixture::createContext(**engine, {"B", path}, manifest("B"), empty_assembly);
        assert(first_context && second_context);
        auto& a = **first_context;
        auto& b = **second_context;
        assert(a.assets().mount({"/Game", *lower, 0}));
        assert(b.assets().mount({"/Game", *lower, 0}));
        const auto mounted = a.assets().mount({"/Game", *higher, 1});
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
        a.assets().unmount(mounted);
        assert(a.assets().open(id)->bytes.size() == low.size());
        assert(frozen.open(id)->bytes.size() == high.size());
        assert(!lifetime.expired());
    }
    assert(lifetime.expired());
    std::puts("PASS direct AssetVfs parity, priority, limit, enumeration, provider lifetime and project isolation");
}
