#pragma once
#include <InspectorAggregateElements.hpp>
#include <tuple>

namespace lux::editor::ui::generated_support
{
    template <class Interaction, class Component, class Value, class Access, bool TIsOptional, class... Factories>
    class TAlternativeFieldElement final : public lux::ui::Element
    {
        using Base = lux::ui::Element;
        template <std::size_t Index> struct TAlternativeAccess final
        {
            Access field;
            auto operator()(auto& component) const noexcept
            {
                auto* value = field(component);
                if constexpr (TIsOptional)
                {
                    using Item = std::remove_reference_t<decltype(**value)>;
                    return value && *value ? std::addressof(**value) : static_cast<Item*>(nullptr);
                }
                else
                    return value ? std::get_if<Index>(value) : nullptr;
            }
        };

    public:
        TAlternativeFieldElement(
            lux::ui::Element& parent,
            lux::ui::ElementId id,
            typename Interaction::Editing& editing,
            typename Interaction::Target target,
            Interaction& interaction,
            typename Interaction::Status& status,
            std::string label,
            bool read_only,
            Access access,
            std::vector<lux::ui::ChoiceOption> options
        )
            : Base(parent.dispatcherRef(), std::move(id)), editing_(editing), target_(target), interaction_(interaction),
              access_(std::move(access)), label_(std::move(label)), read_only_(read_only),
              layout_(parent.dispatcherRef(), lux::ui::ElementId{"alternative"}),
              header_(parent.dispatcherRef(), lux::ui::ElementId{"header"}, lux::ui::ELayoutType::FORM),
              title_(parent.dispatcherRef(), lux::ui::ElementId{"label"}, label_),
              choice_(parent.dispatcherRef(), lux::ui::ElementId{"choice"}, std::move(options)),
              empty_(parent.dispatcherRef(), lux::ui::ElementId{"empty"}, "Empty"),
              connection_(takeConnection<Interaction>(
                  lux::object::LuxObject::connect(
                      std::addressof(choice_),
                      &lux::ui::Choice::edited,
                      [this](lux::ui::EditResult change) noexcept {
                          if (!change.changed)
                              return;
                          requested_ = static_cast<std::size_t>(choice_.value());
                          requested_version_ = editing_.componentVersion(target_, lux::cxx::typeToken<Component>());
                      }
                  ),
                  status
              ))
        {
            if (!this->addSubElement(layout_) ||
                !layout_.addSubElement(header_) ||
                !header_.addSubElement(title_) ||
                !header_.addSubElement(choice_) ||
                !layout_.addSubElement(empty_))
            {
                status = Interaction::constructionFailure();
                return;
            }
            this->setStretch({1, 0});
            rebuild(index(), &status);
        }

    private:
        std::size_t index() const noexcept
        {
            const auto* component =
                static_cast<const Component*>(editing_.component(target_, lux::cxx::typeToken<Component>()));
            const auto* value = component ? access_(*component) : nullptr;
            if constexpr (TIsOptional)
                return value && value->has_value() ? 1 : 0;
            else
                return value ? value->index() : std::variant_npos;
        }
        template <std::size_t I = 0>
        std::unique_ptr<lux::ui::Element> createChild(std::size_t selected, typename Interaction::Status& status)
        {
            if constexpr (I < sizeof...(Factories))
            {
                if (selected == (TIsOptional ? 1 : I))
                {
                    using Factory = std::tuple_element_t<I, std::tuple<Factories...>>;
                    return Factory::create(
                        layout_,
                        lux::ui::ElementId{std::string(this->id().name()) + "/" + std::to_string(selected)},
                        editing_,
                        target_,
                        interaction_,
                        status,
                        label_,
                        read_only_,
                        TAlternativeAccess<I>{access_}
                    );
                }
                return createChild<I + 1>(selected, status);
            }
            else
                return {};
        }
        template <std::size_t I = 0> static bool choose(Value& value, std::size_t selected)
        {
            if constexpr (TIsOptional)
            {
                if (selected == 0)
                    value.reset();
                else if (selected == 1)
                    value.emplace();
                else
                    return false;
                return true;
            }
            else if constexpr (I < sizeof...(Factories))
            {
                if (selected == I)
                {
                    value.template emplace<I>();
                    return true;
                }
                return choose<I + 1>(value, selected);
            }
            else
                return false;
        }
        // The only allocation boundary: construct first, then replace the old subtree.
        void rebuild(std::size_t selected, typename Interaction::Status* initial = nullptr) noexcept
        {
            try
            {
                typename Interaction::Status status;
                auto candidate_layout = std::make_unique<lux::ui::Layout>(
                    this->dispatcherRef(), lux::ui::ElementId{"selected"}
                );
                auto candidate = createChild(selected, status);
                if (candidate && !candidate_layout->addSubElement(*candidate))
                    status = Interaction::constructionFailure();
                if (status)
                {
                    auto attached = child_layout_ ? layout_.replaceSubElement(*child_layout_, *candidate_layout)
                                                  : layout_.addSubElement(*candidate_layout);
                    if (!attached)
                        status = Interaction::constructionFailure();
                }
                if (!status)
                {
                    if (initial)
                        *initial = status;
                    interaction_.fail("Could not connect the alternative controls.");
                    return;
                }
                child_ = std::move(candidate);
                child_layout_ = std::move(candidate_layout);
                selected_ = selected;
                choice_.setValue(static_cast<std::int64_t>(selected));
                choice_.setEnabled(!read_only_);
                empty_.setVisible(!child_);
            }
            catch (const std::bad_alloc&)
            {
                std::terminate();
            }
            catch (...)
            {
                if (initial)
                    *initial = Interaction::constructionFailure();
                interaction_.fail("Could not construct the selected alternative.");
            }
        }
        void update() noexcept override
        {
            if (requested_)
            {
                if (!interaction_.finish())
                    return;
                const auto desired = *requested_;
                const bool stale =
                    requested_version_ != editing_.componentVersion(target_, lux::cxx::typeToken<Component>());
                if (stale)
                    interaction_.fail("The alternative changed before the request was adopted.");
                else if (!read_only_)
                {
                    const auto prepared = interaction_.template mutateField<Component>(
                        target_,
                        this->id().name().data(),
                        label_.c_str(),
                        access_,
                        [desired](auto& next) { return choose(next, desired); }
                    );
                    // This is owner maintenance, after the field owner was visited. Complete the
                    // accepted structural edit before selecting its new child. A failed finish
                    // remains in the original interaction owner for the next maintenance turn.
                    if (prepared)
                        static_cast<void>(interaction_.finish());
                }
                requested_.reset();
                choice_.setValue(static_cast<std::int64_t>(index()));
            }
            const auto selected = index();
            if (selected != selected_ && interaction_.finish())
                rebuild(selected);
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
            layout_.arrange({{}, this->rect().size});
        }
        void draw() noexcept override
        {
            this->drawChild(layout_);
        }
        typename Interaction::Editing& editing_;
        typename Interaction::Target target_;
        Interaction& interaction_;
        Access access_;
        std::string label_;
        bool read_only_;
        lux::ui::Layout layout_, header_;
        lux::ui::Label title_;
        lux::ui::Choice choice_;
        lux::ui::Label empty_;
        std::unique_ptr<lux::ui::Layout> child_layout_;
        std::unique_ptr<lux::ui::Element> child_;
        object::Connection connection_;
        std::size_t selected_{std::variant_npos};
        std::optional<std::size_t> requested_;
        std::uint64_t requested_version_{};
    };
}
