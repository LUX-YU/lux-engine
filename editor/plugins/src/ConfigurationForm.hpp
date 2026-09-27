#pragma once

#include <lux/engine/editor/metadata/ConfigurationValue.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <functional>
#include <limits>
#include <exception>
#include <new>

namespace lux::editor::detail
{
    // Private plugin UI. Only the portable fields are exposed; transient GPU handles are excluded
    // by the same static metadata used by the codec. Accessors resolve the current value on each use.
    class ConfigurationForm final : public lux::ui::Element
    {
    public:
        ConfigurationForm(lux::ui::Element& parent, lux::ui::ElementId id)
            : Element(parent, std::move(id)), layout_(*this, lux::ui::ElementId{"fields"}, lux::ui::ELayoutType::FORM)
        {}

        template <class Accessor> void add(std::string name, Accessor access, EditorResult<void>& status)
        {
            using Value = std::remove_cvref_t<decltype(access())>;
            if (!status)
                return;
            if constexpr (meta::HasTypeStaticInfo<Value>)
            {
                std::apply(
                    [&](const auto&... field) {
                        (add(
                             name.empty() ? std::string(field.name) : name + "." + std::string(field.name),
                             [access, field]() -> auto& { return access().*field.pointer; },
                             status
                         ),
                         ...);
                    },
                    meta::TTypeStaticInfo<Value>::fields
                );
            }
            else if constexpr (std::is_array_v<Value>)
            {
                for (std::size_t index{}; index < std::extent_v<Value>; ++index)
                    add(
                        name + "[" + std::to_string(index) + "]",
                        [access, index]() -> auto& { return access()[index]; },
                        status
                    );
            }
            else
            {
                fields_.push_back(std::make_unique<lux::ui::Label>(layout_, lux::ui::ElementId{name + "/label"}, name));
                if constexpr (std::is_same_v<Value, bool>)
                {
                    auto field = std::make_unique<lux::ui::CheckBox>(layout_, lux::ui::ElementId{name}, "", access());
                    auto* control = field.get();
                    connect(
                        *control,
                        [access, control](lux::ui::EditResult result) noexcept {
                            if (result.changed || result.cancelled)
                                access() = control->value();
                        },
                        status
                    );
                    sync_.push_back([access, control] { control->setValue(access()); });
                    fields_.push_back(std::move(field));
                }
                else if constexpr (std::is_enum_v<Value>)
                {
                    const auto* enumeration =
                        meta::ReflectionRegistry::instance().findEnum(lux::cxx::typeToken<Value>().name());
                    if (enumeration && !enumeration->values.empty())
                    {
                        std::vector<lux::ui::ChoiceOption> options;
                        for (const auto& entry : enumeration->values)
                            options.push_back({entry.value, entry.name});
                        auto field = std::make_unique<lux::ui::Choice>(
                            layout_,
                            lux::ui::ElementId{name},
                            std::move(options),
                            static_cast<std::int64_t>(access())
                        );
                        auto* control = field.get();
                        connect(
                            *control,
                            [access, control](lux::ui::EditResult result) noexcept {
                                if (result.changed || result.cancelled)
                                    access() = static_cast<Value>(control->value());
                            },
                            status
                        );
                        sync_.push_back([access, control] { control->setValue(static_cast<std::int64_t>(access())); });
                        fields_.push_back(std::move(field));
                    }
                    else
                        numeric<std::underlying_type_t<Value>>(
                            name,
                            [access] { return static_cast<std::underlying_type_t<Value>>(access()); },
                            [access](auto value) { access() = static_cast<Value>(value); },
                            status
                        );
                }
                else if constexpr (requires {
                                       access().bits();
                                       Value::fromBits(access().bits());
                                   })
                {
                    using Bits = typename Value::underlying_type;
                    numeric<Bits>(
                        name,
                        [access] { return access().bits(); },
                        [access](Bits bits) { access() = Value::fromBits(bits); },
                        status
                    );
                }
                else if constexpr (std::is_arithmetic_v<Value>)
                {
                    if (name.ends_with("_version"))
                        fields_.push_back(std::make_unique<lux::ui::Label>(
                            layout_,
                            lux::ui::ElementId{name},
                            std::to_string(access())
                        ));
                    else
                        numeric<Value>(name, access, [access](Value value) { access() = value; }, status);
                }
                else
                    status = lux::cxx::unexpected(EditorFailure{
                        EEditorError::INVALID_ARGUMENT,
                        "configuration.field",
                        0,
                        "No portable field control for " + name
                    });
            }
        }

    private:
        template <class Control, class Callback>
        void connect(Control& control, Callback callback, EditorResult<void>& status)
        {
            auto connection = object::LuxObject::connect(&control, &Control::edited, std::move(callback));
            if (connection)
                connections_.push_back(std::move(*connection));
            else
            {
                if (connection.error() == object::EConnectError::ALLOCATION_FAILURE)
                    std::terminate();
                const bool is_capacity_exhausted = connection.error() == object::EConnectError::CAPACITY_EXHAUSTED;
                status = lux::cxx::unexpected(EditorFailure{
                    is_capacity_exhausted ? EEditorError::CAPACITY : EEditorError::FRONTEND_FAILURE,
                    "configuration.connect"
                });
            }
        }
        template <class Value, class Read, class Write>
        void numeric(const std::string& name, Read read, Write write, EditorResult<void>& status)
        {
            using Number = std::conditional_t<
                std::is_integral_v<Value> && (sizeof(Value) < 4),
                std::conditional_t<std::is_signed_v<Value>, std::int32_t, std::uint32_t>,
                Value>;
            auto field =
                std::make_unique<lux::ui::NumericEdit>(layout_, lux::ui::ElementId{name}, static_cast<Number>(read()));
            lux::ui::NumericSpec spec;
            spec.mode = lux::ui::EScalarEditMode::INPUT;
            spec.minimum = static_cast<Number>(std::numeric_limits<Value>::lowest());
            spec.maximum = static_cast<Number>(std::numeric_limits<Value>::max());
            static_cast<void>(field->setSpec(std::move(spec)));
            auto* control = field.get();
            connect(
                *control,
                [write, control](lux::ui::EditResult result) noexcept {
                    if (result.changed || result.cancelled)
                        write(static_cast<Value>(std::get<Number>(control->value())));
                },
                status
            );
            sync_.push_back([read, control] {
                if (!control->editing())
                    control->setValue(static_cast<Number>(read()));
            });
            fields_.push_back(std::move(field));
        }
        lux::ui::SizeHint sizeHintContent() noexcept override
        {
            return layout_.sizeHint();
        }
        lux::ui::SizeHint measureContent(float width) noexcept override
        {
            return layout_.measure(width);
        }
        void arrangeContent() noexcept override
        {
            layout_.arrange({{}, rect().size});
        }
        void draw() noexcept override
        {
            drawChild(layout_);
        }
        void update() noexcept override
        {
            for (const auto& sync : sync_)
                sync();
        }
        lux::ui::Layout layout_;
        std::vector<std::unique_ptr<lux::ui::Element>> fields_;
        std::vector<std::function<void()>> sync_;
        std::vector<object::Connection> connections_;
    };

    template <class Configuration> ConfigurationEditorRegistration configurationEditor(const char* schema)
    {
        return {
            schema,
            1,
            serialization::makePortableValueCodec<Configuration>(),
            +[](meta::ReflectionRegistry& registry) noexcept {
                return registry.findClass(lux::cxx::typeToken<Configuration>().name());
            },
            +[](lux::ui::Element& parent, lux::ui::ElementId id, ConfigurationValue& value
             ) noexcept -> EditorResult<std::unique_ptr<lux::ui::Element>> {
                try
                {
                    EditorResult<void> status;
                    auto form = std::make_unique<ConfigurationForm>(parent, std::move(id));
                    form->add(
                        "",
                        [&value]() -> Configuration& { return *static_cast<Configuration*>(value.data()); },
                        status
                    );
                    if (!status)
                        return lux::cxx::unexpected(status.error());
                    return form;
                }
                catch (const std::bad_alloc&)
                {
                    std::terminate();
                }
                catch (...)
                {
                    return lux::cxx::unexpected(EditorFailure{EEditorError::FRONTEND_FAILURE, "configuration.create"});
                }
            }
        };
    }
}
