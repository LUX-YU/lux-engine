#pragma once

#include <InspectorControl.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/Table.hpp>

namespace lux::editor::ui::generated_support
{
    template <class Value>
    using NumericStorage = std::conditional_t<
        std::is_floating_point_v<Value>,
        Value,
        std::conditional_t<
            std::is_signed_v<Value>,
            std::conditional_t<(sizeof(Value) <= 4), std::int32_t, std::int64_t>,
            std::conditional_t<(sizeof(Value) <= 4), std::uint32_t, std::uint64_t>>>;

    template <class Value, class Control> auto initialControlValue()
    {
        if constexpr (std::same_as<Control, lux::ui::NumericEdit>)
            return NumericStorage<Value>{};
        else if constexpr (std::same_as<Control, lux::ui::Choice>)
            return std::int64_t{};
        else
            return Value{};
    }

    // One generated field owns its controls. The access function is a value, never a cached field pointer.
    template <class Component, class Value, class Access, class Control>
    class TFieldElement final : public lux::ui::Element
    {
        using Base = lux::ui::Element;

    public:
        template <class... Options>
        TFieldElement(
            lux::ui::Element& parent,
            lux::ui::ElementId id,
            scene::SceneEditing& editing,
            lux::simulation::ecs::Entity target,
            InspectorInteraction& interaction,
            EditorResult<void>& status,
            std::string label,
            bool read_only,
            Access access,
            Options&&... options
        )
            : Base(parent, std::move(id)), editing_(editing), target_(target), interaction_(interaction),
              access_(std::move(access)), label_text_(std::move(label)), read_only_(read_only),
              row_(*this, lux::ui::ElementId{"row"}, lux::ui::ELayoutType::FORM),
              label_(row_, lux::ui::ElementId{"label"}, label_text_), control_(
                                                                          row_,
                                                                          lux::ui::ElementId{"value"},
                                                                          std::forward<Options>(options)...,
                                                                          initialControlValue<Value, Control>()
                                                                      ),
              connection_(takeConnection(
                  lux::object::LuxObject::connect(
                      std::addressof(control_),
                      &Control::edited,
                      [this](lux::ui::EditResult change) noexcept {
                          Value next{};
                          if constexpr (std::same_as<Control, lux::ui::NumericEdit>)
                          {
                              const auto candidate = std::get<NumericStorage<Value>>(control_.value());
                              bool valid = control_.valueValid();
                              if constexpr (std::is_integral_v<Value>)
                                  valid = valid && std::in_range<Value>(candidate);
                              if (!valid)
                              {
                                  interaction_.fail("The field value is outside its declared range.");
                                  sync(true);
                                  return;
                              }
                              next = static_cast<Value>(std::get<NumericStorage<Value>>(control_.value()));
                          }
                          else if constexpr (std::same_as<Control, lux::ui::Choice>)
                              next = static_cast<Value>(control_.value());
                          else
                              next = control_.value();
                          if (!read_only_ && scene::TFieldValue<Value>::valid(next))
                              static_cast<void>(interaction_.template apply<Component, Value>(
                                  target_,
                                  this->id().name().data(),
                                  label_text_.c_str(),
                                  access_,
                                  next,
                                  change
                              ));
                          else
                              interaction_.fail("The field value is invalid or read-only.");
                          sync(true);
                      }
                  ),
                  status
              ))
        {
            this->setStretch({1, 0});
            sync(true);
        }
        Control& control() noexcept
        {
            return control_;
        }

    private:
        void sync(bool force)
        {
            const auto version = editing_.componentVersion(target_, lux::cxx::typeToken<Component>());
            if (!force && synchronized_ && version == version_)
            {
                control_.setEnabled(
                    available_ && !read_only_ && editing_.writeRestriction().empty() &&
                    (!interaction_.active() || interaction_.ownsField(this->id().name()))
                );
                return;
            }
            const auto* component =
                static_cast<const Component*>(editing_.component(target_, lux::cxx::typeToken<Component>()));
            const auto* value = component ? access_(*component) : nullptr;
            version_ = version;
            synchronized_ = true;
            available_ = value != nullptr;
            const bool enabled = value && !read_only_ && editing_.writeRestriction().empty() &&
                                 (!interaction_.active() || interaction_.ownsField(this->id().name()));
            control_.setEnabled(enabled);
            if (!value || (!force && interaction_.ownsField(this->id().name())))
                return;
            if constexpr (std::same_as<Control, lux::ui::NumericEdit>)
                control_.setValue(NumericStorage<Value>(*value));
            else if constexpr (std::same_as<Control, lux::ui::Choice>)
                control_.setValue(static_cast<std::int64_t>(*value));
            else if (control_.value() != *value)
                control_.setValue(*value);
        }
        lux::ui::SizeHint sizeHintContent() noexcept override
        {
            return row_.sizeHint();
        }
        lux::ui::SizeHint measureContent(float width) noexcept override
        {
            return row_.measure(width);
        }
        void arrangeContent() noexcept override
        {
            row_.arrange({{}, this->rect().size});
        }
        void draw() noexcept override
        {
            this->drawChild(row_);
        }
        void update() noexcept override
        {
            sync(false);
        }

        scene::SceneEditing& editing_;
        lux::simulation::ecs::Entity target_;
        InspectorInteraction& interaction_;
        Access access_;
        std::string label_text_;
        bool read_only_;
        std::uint64_t version_{};
        bool synchronized_{}, available_{};
        lux::ui::Layout row_;
        lux::ui::Label label_;
        Control control_;
        object::Connection connection_;
    };

    // Custom fields keep their specialized ImGui internals,
    // but have a persistent content identity and never create a window.
    template <class Component, class Value, class Access, auto Draw>
    class TCompositeFieldElement final : public lux::ui::Element
    {
        using Base = lux::ui::Element;

    public:
        TCompositeFieldElement(
            lux::ui::Element& parent,
            lux::ui::ElementId id,
            scene::SceneEditing& editing,
            lux::simulation::ecs::Entity target,
            InspectorInteraction& interaction,
            EditorResult<void>& status,
            std::string label,
            bool read_only,
            Access access,
            float rows
        )
            : Base(parent, std::move(id)), editing_(editing), target_(target), interaction_(interaction),
              access_(std::move(access)), label_(std::move(label)), read_only_(read_only), rows_(rows)
        {
            this->setStretch({1, 0});
            synchronize();
        }

        void finishEdit(bool cancel = false) noexcept override
        {
            if (!interaction_.ownsField(this->id().name()))
                return;
            if (cancel && original_)
                value_ = *original_;
            const lux::ui::EditResult change{cancel, false, !cancel, cancel};
            if (interaction_.template apply<Component, Value>(
                    target_,
                    this->id().name().data(),
                    label_.c_str(),
                    access_,
                    value_,
                    change
                ))
                original_.reset();
            synchronize();
        }

    private:
        lux::ui::SizeHint sizeHintContent() noexcept override
        {
            const auto height = rows_ * (ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2 + 4);
            return {{120, height}, {360, height}, {std::numeric_limits<float>::infinity(), height}};
        }
        void draw() noexcept override
        {
            auto table = lux::ui::TableScope({lux::ui::ElementIdView{"field"}, 2, false, false, false, 110});
            if (!table.visible() || !available_)
                return;
            const bool active = interaction_.ownsField(this->id().name());
            const bool disabled =
                read_only_ || !editing_.writeRestriction().empty() || (interaction_.active() && !active);
            lux::ui::propertyRow(label_.c_str());
            const auto previous = interaction_.readOnly();
            interaction_.setReadOnly(disabled);
            ImGui::BeginDisabled(disabled);
            ImGui::PushID(this->id().name().data());
            auto change = Draw(value_, interaction_);
            const bool cancel = active && ImGui::IsKeyPressed(ImGuiKey_Escape);
            ImGui::PopID();
            ImGui::EndDisabled();
            interaction_.setReadOnly(previous);
            if (disabled)
                return;
            if (cancel)
            {
                finishEdit(true);
                return;
            }
            if (change.changed || change.committed || change.cancelled)
            {
                if (!original_)
                {
                    const auto* component =
                        static_cast<const Component*>(editing_.component(target_, lux::cxx::typeToken<Component>()));
                    if (component && access_(*component))
                        original_ = *access_(*component);
                }
                if (!interaction_.template apply<Component, Value>(
                        target_,
                        this->id().name().data(),
                        label_.c_str(),
                        access_,
                        value_,
                        change
                    ))
                    synchronize();
                if (!interaction_.ownsField(this->id().name()))
                    original_.reset();
            }
        }
        void synchronize(bool force = true) noexcept
        {
            const auto version = editing_.componentVersion(target_, lux::cxx::typeToken<Component>());
            if (!force && synchronized_ && version == version_)
                return;
            const auto* component =
                static_cast<const Component*>(editing_.component(target_, lux::cxx::typeToken<Component>()));
            const auto* field = component ? access_(*component) : nullptr;
            available_ = field != nullptr;
            version_ = version;
            synchronized_ = true;
            if (field)
                value_ = *field;
        }
        void update() noexcept override
        {
            if (!interaction_.ownsField(this->id().name()))
                synchronize(false);
        }
        scene::SceneEditing& editing_;
        lux::simulation::ecs::Entity target_;
        InspectorInteraction& interaction_;
        Access access_;
        std::string label_;
        bool read_only_;
        float rows_;
        Value value_{};
        std::optional<Value> original_;
        std::uint64_t version_{};
        bool available_{}, synchronized_{};
    };
}
