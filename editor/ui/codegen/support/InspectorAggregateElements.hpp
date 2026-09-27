#pragma once
#include <InspectorElementFields.hpp>
#include <lux/engine/ui/Root.hpp>

namespace lux::editor::ui::generated_support
{
    // Fixed fields in a dynamic row still own a persistent, measured content tree.
    class FieldGroup final : public lux::ui::Element
    {
    public:
        FieldGroup(lux::ui::Element& parent, lux::ui::ElementId id)
            : lux::ui::Element(parent, std::move(id)), layout_(*this, lux::ui::ElementId{"fields"})
        {
            setStretch({1, 0});
        }
        lux::ui::Layout& layout() noexcept
        {
            return layout_;
        }
        void add(std::unique_ptr<lux::ui::Element> field)
        {
            fields_.push_back(std::move(field));
        }

    private:
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
        lux::ui::Layout layout_;
        std::vector<std::unique_ptr<lux::ui::Element>> fields_;
    };

    template <class Component, class Value, class Access, class ItemFactory>
    class TSequenceFieldElement final : public lux::ui::Element
    {
        using Base = lux::ui::Element;
        enum class EAction
        {
            NONE,
            PREVIOUS,
            NEXT,
            ADD,
            REMOVE,
            UP,
            DOWN,
            SET_BIT
        };
        static constexpr bool packed_bits = std::same_as<Value, std::vector<bool>>;
        struct ItemAccess final
        {
            Access field;
            std::size_t index{}, count{};
            auto operator()(auto& component) const noexcept
            {
                auto* value = field(component);
                using Item = std::remove_reference_t<decltype(*value->begin())>;
                if (!value || value->size() != count || index >= count)
                    return static_cast<Item*>(nullptr);
                return std::addressof(*std::next(value->begin(), static_cast<std::ptrdiff_t>(index)));
            }
        };
        class Row final : public lux::ui::Element
        {
        public:
            Row(TSequenceFieldElement& owner, std::size_t index, std::size_t count, EditorResult<void>& status)
                : lux::ui::Element(owner.layout_, lux::ui::ElementId{"row/" + std::to_string(index)}), owner_(owner),
                  layout_(*this, lux::ui::ElementId{"row"}),
                  actions_(layout_, lux::ui::ElementId{"actions"}, lux::ui::ELayoutType::HORIZONTAL),
                  up_(actions_, lux::ui::ElementId{"up"}, "Up"), down_(actions_, lux::ui::ElementId{"down"}, "Down"),
                  remove_(actions_, lux::ui::ElementId{"remove"}, "Remove")
            {
                this->setStretch({1, 0});
                const auto identity = std::string(owner.id().name()) + "[" + std::to_string(index) + "]";
                if constexpr (packed_bits)
                {
                    auto control = std::make_unique<lux::ui::CheckBox>(
                        layout_,
                        lux::ui::ElementId{identity},
                        "[" + std::to_string(index) + "]",
                        (*owner.value())[index]
                    );
                    control->setEnabled(!owner.read_only_);
                    auto* checkbox = control.get();
                    connections_[3] = takeConnection(
                        lux::object::LuxObject::connect(
                            std::addressof(*control),
                            &lux::ui::CheckBox::edited,
                            [&owner, index, checkbox](lux::ui::EditResult change) noexcept {
                                if (change.changed)
                                {
                                    owner.request(EAction::SET_BIT, index);
                                    owner.bit_ = checkbox->value();
                                }
                            }
                        ),
                        status
                    );
                    field_ = std::move(control);
                }
                else
                    field_ = ItemFactory::create(
                        layout_,
                        lux::ui::ElementId{identity},
                        owner.editing_,
                        owner.target_,
                        owner.interaction_,
                        status,
                        "[" + std::to_string(index) + "]",
                        owner.read_only_,
                        ItemAccess{owner.access_, index, count}
                    );
                index_ = index;
                up_.setEnabled(index != 0);
                down_.setEnabled(index + 1 < count);
                actions_.setEnabled(!owner.read_only_);
                connections_[0] = takeConnection(
                    lux::object::LuxObject::connect(
                        std::addressof(up_),
                        &lux::ui::Button::activated,
                        [&owner, index]() noexcept { owner.request(EAction::UP, index); }
                    ),
                    status
                );
                connections_[1] = takeConnection(
                    lux::object::LuxObject::connect(
                        std::addressof(down_),
                        &lux::ui::Button::activated,
                        [&owner, index]() noexcept { owner.request(EAction::DOWN, index); }
                    ),
                    status
                );
                connections_[2] = takeConnection(
                    lux::object::LuxObject::connect(
                        std::addressof(remove_),
                        &lux::ui::Button::activated,
                        [&owner, index]() noexcept { owner.request(EAction::REMOVE, index); }
                    ),
                    status
                );
            }

        private:
            void update() noexcept override
            {
                if constexpr (packed_bits)
                {
                    const auto* current = owner_.value();
                    auto* control = static_cast<lux::ui::CheckBox*>(field_.get());
                    control->setEnabled(current && index_ < current->size() && !owner_.read_only_);
                    if (current && index_ < current->size())
                        control->setValue((*current)[index_]);
                }
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
            TSequenceFieldElement& owner_;
            lux::ui::Layout layout_, actions_;
            lux::ui::Button up_, down_, remove_;
            std::unique_ptr<lux::ui::Element> field_;
            std::array<object::Connection, 4> connections_;
            std::size_t index_{};
        };

    public:
        TSequenceFieldElement(
            lux::ui::Element& parent,
            lux::ui::ElementId id,
            scene::SceneEditing& editing,
            lux::simulation::ecs::Entity target,
            InspectorInteraction& interaction,
            EditorResult<void>& status,
            std::string label,
            bool read_only,
            Access access
        )
            : Base(parent, std::move(id)), editing_(editing), target_(target), interaction_(interaction),
              access_(std::move(access)), label_text_(std::move(label)), read_only_(read_only),
              layout_(*this, lux::ui::ElementId{"sequence"}), title_(layout_, lux::ui::ElementId{"title"}, label_text_),
              pages_(layout_, lux::ui::ElementId{"pages"}, lux::ui::ELayoutType::HORIZONTAL),
              previous_(pages_, lux::ui::ElementId{"previous"}, "Previous page"),
              next_(pages_, lux::ui::ElementId{"next"}, "Next page"), add_(pages_, lux::ui::ElementId{"add"}, "Add")
        {
            this->setStretch({1, 0});
            connections_[0] = takeConnection(
                lux::object::LuxObject::connect(
                    std::addressof(previous_),
                    &lux::ui::Button::activated,
                    [this]() noexcept { request(EAction::PREVIOUS); }
                ),
                status
            );
            connections_[1] = takeConnection(
                lux::object::LuxObject::connect(
                    std::addressof(next_),
                    &lux::ui::Button::activated,
                    [this]() noexcept { request(EAction::NEXT); }
                ),
                status
            );
            connections_[2] = takeConnection(
                lux::object::LuxObject::connect(
                    std::addressof(add_),
                    &lux::ui::Button::activated,
                    [this]() noexcept { request(EAction::ADD); }
                ),
                status
            );
            rebuild(&status);
        }

    private:
        const Value* value() const noexcept
        {
            const auto* component =
                static_cast<const Component*>(editing_.component(target_, lux::cxx::typeToken<Component>()));
            return component ? access_(*component) : nullptr;
        }
        void request(EAction action, std::size_t index = 0) noexcept
        {
            action_ = action;
            action_index_ = index;
            action_version_ = editing_.componentVersion(target_, lux::cxx::typeToken<Component>());
        }
        // Fallible subtree factory. A failed candidate leaves the old controls alive.
        bool rebuild(EditorResult<void>* initial = nullptr) noexcept
        {
            try
            {
                EditorResult<void> status;
                const auto* current = value();
                const auto count = current ? current->size() : 0;
                const auto first = std::min(first_, count ? (count - 1) / page_size * page_size : 0);
                std::vector<std::unique_ptr<Row>> candidate;
                candidate.reserve(std::min(page_size, count - first));
                for (auto index = first; index < std::min(count, first + page_size); ++index)
                    candidate.push_back(std::make_unique<Row>(*this, index, count, status));
                if (!status)
                {
                    if (initial)
                        *initial = status;
                    interaction_.fail("Could not connect the container controls.");
                    return false;
                }
                rows_ = std::move(candidate);
                first_ = first;
                count_ = count;
                previous_.setEnabled(first != 0);
                next_.setEnabled(first + page_size < count);
                add_.setEnabled(!read_only_);
                return true;
            }
            catch (const std::bad_alloc&)
            {
                std::terminate();
            }
            catch (...)
            {
                if (initial)
                    *initial = lux::cxx::unexpected(EditorFailure{EEditorError::FRONTEND_FAILURE, "inspector.create"});
                interaction_.fail("Could not construct the container controls.");
            }
            return false;
        }
        void update() noexcept override
        {
            bool changed{};
            if (action_ != EAction::NONE)
            {
                if (!interaction_.finish())
                    return;
                const auto action = std::exchange(action_, EAction::NONE);
                const auto version = editing_.componentVersion(target_, lux::cxx::typeToken<Component>());
                if (version != action_version_)
                    interaction_.fail("The container changed before this action was adopted.");
                else if (action == EAction::PREVIOUS || action == EAction::NEXT)
                {
                    first_ = action == EAction::PREVIOUS ? (first_ >= page_size ? first_ - page_size : 0)
                                                         : first_ + page_size;
                    changed = true;
                }
                else if (!read_only_)
                {
                    changed = interaction_.template mutateField<
                        Component>(target_, this->id().name().data(), label_text_.c_str(), access_, [&](auto& next) {
                        if (action == EAction::ADD)
                        {
                            next.emplace_back();
                            return true;
                        }
                        if (next.size() != count_ || action_index_ >= count_)
                            return false;
                        if constexpr (packed_bits)
                            if (action == EAction::SET_BIT)
                            {
                                next[action_index_] = bit_;
                                return true;
                            }
                        auto position = std::next(next.begin(), static_cast<std::ptrdiff_t>(action_index_));
                        if (action == EAction::REMOVE)
                        {
                            next.erase(position);
                            return true;
                        }
                        if ((action == EAction::UP && action_index_ == 0) ||
                            (action == EAction::DOWN && action_index_ + 1 == count_))
                            return false;
                        auto other = std::next(
                            next.begin(),
                            static_cast<std::ptrdiff_t>(action == EAction::UP ? action_index_ - 1 : action_index_ + 1)
                        );
                        std::iter_swap(position, other);
                        return true;
                    });
                }
            }
            const auto* current = value();
            if ((changed || count_ != (current ? current->size() : 0)) && interaction_.finish())
                rebuild();
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
        static constexpr std::size_t page_size = 16;
        scene::SceneEditing& editing_;
        lux::simulation::ecs::Entity target_;
        InspectorInteraction& interaction_;
        Access access_;
        std::string label_text_;
        bool read_only_;
        lux::ui::Layout layout_;
        lux::ui::Label title_;
        lux::ui::Layout pages_;
        lux::ui::Button previous_, next_, add_;
        std::vector<std::unique_ptr<Row>> rows_;
        std::array<object::Connection, 3> connections_;
        std::size_t first_{}, count_{}, action_index_{};
        EAction action_{};
        std::uint64_t action_version_{};
        bool bit_{};
    };
}
