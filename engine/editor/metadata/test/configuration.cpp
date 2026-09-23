#include <lux/engine/editor/metadata/ConfigurationValue.hpp>
#include <lux/engine/meta/TypeStaticInfo.hpp>
#include <lux/engine/ui/Context.hpp>
#include <lux/engine/ui/Frame.hpp>
#include <lux/engine/ui/Theme.hpp>
#include <imgui.h>

#include <cassert>
#include <string>
#include <tuple>
#include <vector>

struct Configuration final
{
    std::string label;
    std::vector<std::uint32_t> layers;
};

namespace lux::meta
{
template <> struct TypeStaticInfo<Configuration>
{
    static constexpr bool available = true;
    static constexpr auto fields = std::make_tuple(
        typeStaticField<&Configuration::label>("label"), typeStaticField<&Configuration::layers>("layers")
    );
};
}

void registerConfiguration(lux::meta::ReflectionRegistry &registry,
                           lux::meta::qual_type_index_fix_list &)
{
    using namespace lux;
    auto value = std::make_unique<meta::RefClass>();
    value->name = "Configuration";
    value->full_name = cxx::type_name<Configuration>();
    value->hash = cxx::type_hash<Configuration>();
    value->type = meta::ref_type_of_v<Configuration>;
    value->type.ptr = value.get();
    value->construct = [](void *storage) { std::construct_at(static_cast<Configuration *>(storage)); };
    value->destruct = [](void *storage) { std::destroy_at(static_cast<Configuration *>(storage)); };
    registry.registerClass(std::move(value));
}

int main()
{
    using namespace lux;
    meta::ReflectionRegistry::initRegistry();
    std::weak_ptr<const void> lifetime;
    {
        auto code = std::make_shared<int>(7);
        lifetime = code;
        auto draft = meta::ReflectionRegistry::beginDraft();
        assert(draft.append(&registerConfiguration, code));
        assert(!meta::ReflectionRegistry::instance().findClass(cxx::type_name<Configuration>()));
        assert(draft.prepareCommit());
        assert(draft.commit());
        const editor::ConfigurationEditorRegistration registration{
            "test.configuration", 1, serialization::makePortableValueCodec<Configuration>(),
            [](meta::ReflectionRegistry &registry) noexcept { return registry.findClass(cxx::type_name<Configuration>()); },
            +[](ui::Frame &frame, void *object) noexcept {
                if (!frame.smallButton("Apply configuration")) return false;
                auto &value = *static_cast<Configuration *>(object);
                value.label = "owned nontrivial configuration";
                value.layers = {2, 3, 5};
                return true;
            }
        };
        auto first = editor::ConfigurationValue::create(registration, code);
        auto second = editor::ConfigurationValue::create(registration, code);
        assert(first && second);
        auto &typed = *static_cast<Configuration *>(first->data());
        auto context = ui::Context::create();
        assert(context);
        bool edited{};
        for (unsigned turn{}; turn < 4; ++turn)
        {
            ui::Frame frame(*context, ui::Theme::luxDark(), {{640, 480}, 1.0F / 60.0F});
            edited |= first->edit(frame);
            if (turn == 1)
            {
                const auto minimum = ImGui::GetItemRectMin();
                const auto maximum = ImGui::GetItemRectMax();
                ImGui::GetIO().AddMousePosEvent((minimum.x + maximum.x) / 2, (minimum.y + maximum.y) / 2);
                ImGui::GetIO().AddMouseButtonEvent(0, true);
            }
            if (turn == 2) ImGui::GetIO().AddMouseButtonEvent(0, false);
            frame.finish();
        }
        assert(edited && typed.label == "owned nontrivial configuration");
        assert(typed.layers == std::vector<std::uint32_t>({2, 3, 5}));
        std::vector<std::byte> bytes;
        assert(first->encode(bytes));
        assert(second->decode(bytes));
        const auto &roundtrip = *static_cast<const Configuration *>(second->data());
        assert(roundtrip.label == typed.label && roundtrip.layers == typed.layers);
        bytes.pop_back();
        assert(!second->decode(bytes));
        assert(roundtrip.label == typed.label && roundtrip.layers == typed.layers);
        auto duplicate = meta::ReflectionRegistry::beginDraft();
        assert(!duplicate.append(&registerConfiguration, code));
        assert(!duplicate.prepareCommit());
    }
    assert(!lifetime.expired());
    meta::ReflectionRegistry::destroyRegistry();
    assert(lifetime.expired());
}
