#ifdef NDEBUG
#undef NDEBUG
#endif

#include <lux/engine/editor/application/EditorApplication.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/resource/asset/storage/AssetProvider.hpp>
#include <lux/engine/simulation/SimulationSystemRegistry.hpp>
#include <lux/engine/simulation/ecs/ComponentSchemaSet.hpp>
#include <lux/engine/ui/Pane.hpp>

#include <array>
#include <cassert>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace
{
    [[nodiscard]] lux::asset::AssetId assetId(std::uint8_t tail)
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes.back() = tail;
        return lux::asset::AssetId{bytes};
    }

    class Provider final : public lux::asset::IAssetProvider
    {
    public:
        [[nodiscard]] std::optional<lux::asset::AssetId> resolve(std::string_view path) const override
        {
            return path == "probe" ? std::optional{assetId(1U)} : std::nullopt;
        }

        [[nodiscard]] bool contains(const lux::asset::AssetId& id) const override
        {
            return id == assetId(1U);
        }

        [[nodiscard]] lux::cxx::expected<lux::asset::AssetBlob, lux::asset::EAssetStorageError>
        open(const lux::asset::AssetId&) const override
        {
            return lux::cxx::unexpected(lux::asset::EAssetStorageError::NOT_FOUND);
        }

        void enumerate(const std::function<void(const lux::asset::ProviderEntry&)>& fn) const override
        {
            fn(lux::asset::ProviderEntry{assetId(1U), 1U, "probe", false});
        }

        [[nodiscard]] std::optional<std::string> pathOf(const lux::asset::AssetId& id) const override
        {
            return id == assetId(1U) ? std::optional<std::string>{"probe"} : std::nullopt;
        }
    };

    [[nodiscard]] lux::scene::SceneMetaManager emptySceneMeta()
    {
        auto schemas = lux::simulation::ecs::ComponentSchemaSet::build({});
        assert(schemas);
        auto meta = lux::scene::SceneMetaManager::build({
            std::move(*schemas),
            lux::simulation::SimulationSystemRegistry{},
            {},
            {},
            {}
        });
        assert(meta);
        return std::move(*meta);
    }

    struct ToolState final
    {
        int stop_count{};
        int destruction_count{};
    };

    class Tool final
    {
    public:
        explicit Tool(ToolState& state) noexcept : state_(state)
        {
        }

        ~Tool() noexcept
        {
            ++state_.destruction_count;
        }

        void requestStop() noexcept { ++state_.stop_count; }

    private:
        ToolState& state_;
    };

    struct FrozenTool final {};

    class TestPane final : public lux::object::Object<TestPane, lux::ui::Pane>
    {
    public:
        explicit TestPane(lux::object::ObjectDispatcherRef dispatcher)
            : Object(
                  std::move(dispatcher),
                  lux::ui::PaneId{"application.test"},
                  lux::ui::PaneTypeId{"application.test"},
                  "Application Test"
              )
        {
        }

    protected:
        void draw(lux::ui::Frame&, lux::ui::PaneDrawContext&) override {}
    };
}

int main()
{
    lux::meta::ReflectionRegistry::initRegistry();
    ToolState tool_state;
    auto provider = std::make_shared<Provider>();
    auto application = lux::editor::EditorApplication::create({
        {2U, 8U, 8U, {8U}, lux::process::BlockingSchedulerConfig{1U, 8U}},
        {8U},
        emptySceneMeta(),
        {{"/Game", provider, 0}}
    });
    assert(application);
    auto pre_start = (*application)->context();
    assert(!pre_start);
    assert((*application)->installTool<Tool>(tool_state));

    auto started = (*application)->start();
    assert(started);
    assert((*application)->installTool<FrozenTool>().error().code ==
        lux::editor::EToolsetError::FROZEN);

    auto context = (*application)->context();
    assert(context);
    auto& capabilities = context->get();
    assert(capabilities.vfs().resolve("/Game/probe") == assetId(1U));
    assert(capabilities.assetRead());
    assert(std::addressof(capabilities.execution()) != nullptr);
    assert(std::addressof(capabilities.tasks()) != nullptr);
    assert(std::addressof(capabilities.selection()) != nullptr);
    {
        TestPane pane{capabilities.ui().dispatcherRef()};
        auto registration = capabilities.ui().registerPane(pane);
        assert(registration);
    }

    assert((*application)->shutdown());
    assert(tool_state.stop_count == 1);
    assert(tool_state.destruction_count == 1);
    const auto stopped_install = (*application)->installTool<FrozenTool>();
    assert(!stopped_install);
    assert(stopped_install.error().code == lux::editor::EToolsetError::STOPPING);
    application->reset();
    lux::meta::ReflectionRegistry::destroyRegistry();
    return 0;
}
