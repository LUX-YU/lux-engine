#include <cassert>
#include <cstdio>
#include <lux/engine/EngineContext.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>
#include <lux/engine/resource/asset/storage/pak/PakAssetProvider.hpp>
#include <type_traits>

using namespace lux;
using namespace lux::editor;
namespace
{
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
    static_assert(!std::is_move_constructible_v<EditorContext>);
    static_assert(!std::is_copy_constructible_v<EditorServiceRegistrar>);
    static_assert(!std::is_move_constructible_v<EditorUiRegistrar>);
    static_assert(!std::is_copy_constructible_v<SceneToolRegistrar>);
    auto& queue = object::ObjectRuntime::instance();
    auto engine = engine::EngineContext::create({1, 64, 64, {32}}, {0, 64});
    assert(queue.isCurrent() && engine);
    std::vector<int> destroyed;
    unsigned constructed{}, attempts{}, tools{};
    {
        EditorContext context(**engine, {"First", std::filesystem::current_path()});
        assert(context.services().registerFactory<Service>(
            [&](EditorContext&) -> FrameworkResult<std::unique_ptr<Service>>
            {
                ++constructed;
                return std::make_unique<Service>(destroyed, 1);
            }
        ));
        assert(!context.services().registerFactory<Service>(
            [](EditorContext&) -> FrameworkResult<std::unique_ptr<Service>>
            { return cxx::unexpected(FrameworkFailure{EFrameworkError::FACTORY_FAILED, "unused"}); }
        ));
        assert(context.services().registerFactory<Other>(
            [&](EditorContext& context) -> FrameworkResult<std::unique_ptr<Other>>
            {
                assert(context.service<Service>());
                return std::make_unique<Other>(destroyed);
            }
        ));
        assert(context.services().registerFactory<Recursive>(
            [](EditorContext& context) -> FrameworkResult<std::unique_ptr<Recursive>>
            {
                auto nested = context.service<Recursive>();
                assert(!nested && nested.error().code == EFrameworkError::RECURSIVE_CONSTRUCTION);
                return cxx::unexpected(std::move(nested.error()));
            }
        ));
        assert(context.services().registerFactory<Retry>(
            [&](EditorContext&) -> FrameworkResult<std::unique_ptr<Retry>>
            {
                if (++attempts == 1)
                {
                    return cxx::unexpected(FrameworkFailure{EFrameworkError::FACTORY_FAILED, "first attempt"});
                }
                return std::make_unique<Retry>();
            }
        ));
        assert(context.sceneTools().registerFactory<Tools>(
            &match,
            [&](EditorContext&, const world::WorldDescription&) -> FrameworkResult<std::unique_ptr<Tools>>
            { return std::make_unique<Tools>(++tools); }
        ));
        assert(constructed == 0);
        assert(!context.service<Service>());
        context.freeze();
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
        assert(!context.services().registerFactory<Missing>(
            [](EditorContext&) -> FrameworkResult<std::unique_ptr<Missing>> { return std::make_unique<Missing>(); }
        ));
        EditorContext second(**engine, {"Second", std::filesystem::current_path()});
        auto factory = [](EditorContext&, const world::WorldDescription&) -> FrameworkResult<std::unique_ptr<Tools>>
        { return std::make_unique<Tools>(9); };
        assert(second.sceneTools().registerFactory<Tools>(&match, factory));
        assert(second.sceneTools().registerFactory<Tools>(&alsoMatch, factory));
        second.freeze();
        auto ambiguous = second.sceneTools().create<Tools>(second, world);
        assert(!ambiguous && ambiguous.error().code == EFrameworkError::AMBIGUOUS);
        SceneToolRegistrar empty;
        empty.freeze();
        auto missing = empty.create<Tools>(second, world);
        assert(!missing && missing.error().code == EFrameworkError::NOT_FOUND);
    }
    assert((destroyed == std::vector<int>{2, 1}));
    std::puts("PASS lazy services, frozen registration, recursion, retry, reverse destruction, typed tool rules");
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
        EditorContext a(**engine, {"A", path});
        EditorContext b(**engine, {"B", path});
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
