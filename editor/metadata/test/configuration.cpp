#include <lux/engine/editor/configuration/ConfigurationValue.hpp>
#include <lux/engine/editor/metadata/EditorPluginExports.hpp>
#include <lux/engine/meta/TypeStaticInfo.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/Controls.hpp>
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
    template <> struct TTypeStaticInfo<Configuration>
    {
        static constexpr bool available = true;
        static constexpr auto fields = std::make_tuple(
            typeStaticField<&Configuration::label>("label"),
            typeStaticField<&Configuration::layers>("layers")
        );
    };
}

void registerConfiguration(lux::meta::ReflectionRegistry& registry, lux::meta::qual_type_index_fix_list&)
{
    using namespace lux;
    auto value = std::make_unique<meta::RefClass>();
    value->name = "Configuration";
    value->full_name = cxx::type_name<Configuration>();
    value->hash = cxx::type_hash<Configuration>();
    value->type = meta::ref_type_of_v<Configuration>;
    value->type.ptr = value.get();
    value->construct = [](void* storage) { std::construct_at(static_cast<Configuration*>(storage)); };
    value->destruct = [](void* storage) { std::destroy_at(static_cast<Configuration*>(storage)); };
    registry.registerClass(std::move(value));
}

class ConfigurationElement final : public lux::ui::Element
{
public:
    ConfigurationElement(lux::ui::Element& parent, lux::ui::ElementId id, lux::editor::ConfigurationValue& value)
        : Element(parent, std::move(id)), value_(value),
          button_(*this, lux::ui::ElementId{"apply"}, "Apply configuration")
    {
        auto connected = lux::object::LuxObject::connect(&button_, &lux::ui::Button::activated, [this]() noexcept {
            auto& value = *static_cast<Configuration*>(value_.data());
            value.label = "owned nontrivial configuration";
            value.layers = {2, 3, 5};
        });
        assert(connected);
        connection_ = std::move(*connected);
    }
    lux::ui::Button& button() noexcept
    {
        return button_;
    }

private:
    lux::ui::SizeHint sizeHintContent() noexcept override
    {
        return button_.sizeHint();
    }
    lux::ui::SizeHint measureContent(float width) noexcept override
    {
        return button_.measure(width);
    }
    void arrangeContent() noexcept override
    {
        button_.arrange({{}, rect().size});
    }
    void draw() noexcept override
    {
        drawChild(button_);
    }
    lux::editor::ConfigurationValue& value_;
    lux::ui::Button button_;
    lux::object::Connection connection_;
};

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
            "test.configuration",
            1,
            serialization::makePortableValueCodec<Configuration>(),
            [](meta::ReflectionRegistry& registry) noexcept {
                return registry.findClass(cxx::type_name<Configuration>());
            },
            +[](ui::Element& parent, ui::ElementId id, editor::ConfigurationValue& value
             ) noexcept -> editor::EditorResult<std::unique_ptr<ui::Element>> {
                return std::make_unique<ConfigurationElement>(parent, std::move(id), value);
            }
        };
        auto first = editor::ConfigurationValue::create(
            {registration.schema_name, registration.schema_version, registration.codec, registration.reflection},
            code
        );
        auto second = editor::ConfigurationValue::create(
            {registration.schema_name, registration.schema_version, registration.codec, registration.reflection},
            code
        );
        assert(first && second);
        auto& typed = *static_cast<Configuration*>(first->data());
        auto messages_created = object::ObjectMessageQueue::create(64);
        assert(messages_created);
        auto messages = std::move(*messages_created);
        auto context = ui::Root::create(messages.dispatcherRef());
        assert(context);
        ui::Pane pane(**context, ui::PaneId{"configuration"}, ui::PaneTypeId{"test"}, "Configuration");
        ui::Layout layout(pane, ui::ElementId{"content"}, ui::ELayoutType::VERTICAL);
        pane.setContent(layout);
        auto created = registration.create(layout, ui::ElementId{"configuration"}, *first);
        assert(created);
        auto& element = static_cast<ConfigurationElement&>(**created);
        ui::DrawData snapshot;
        for (unsigned turn{}; turn < 4; ++turn)
        {
            assert((*context)->update({{640, 480}, 1.0F / 60.0F}, &snapshot));
            if (turn == 1)
            {
                const auto origin = element.button().contentOrigin();
                const auto size = element.button().rect().size;
                assert((*context)->feedInput(ui::PointerMove{{origin.x + size.width / 2, origin.y + size.height / 2}}));
                assert((*context)->feedInput(ui::PointerButton{ui::EPointerButton::LEFT, true}));
            }
            if (turn == 2)
                assert((*context)->feedInput(ui::PointerButton{ui::EPointerButton::LEFT, false}));
            assert((*context)->update({}, nullptr));
        }
        assert(typed.label == "owned nontrivial configuration");
        assert(typed.layers == std::vector<std::uint32_t>({2, 3, 5}));
        std::vector<std::byte> bytes;
        assert(first->encode(bytes));
        assert(second->decode(bytes));
        const auto& roundtrip = *static_cast<const Configuration*>(second->data());
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
