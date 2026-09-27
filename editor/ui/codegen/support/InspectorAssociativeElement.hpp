#pragma once
#include <InspectorAlternativeElement.hpp>

namespace lux::editor::ui::generated_support
{
    template <class Component, class Value, class Access, class ItemFactory>
    class TAssociativeFieldElement final : public lux::ui::Element
    {
        using Base = lux::ui::Element;
        using Key = typename Value::key_type;
        static constexpr bool mapping = requires { typename Value::mapped_type; };
        static constexpr bool text_key = std::same_as<Key, std::string>;
        using KeyControl = std::conditional_t<text_key, lux::ui::TextEdit, lux::ui::NumericEdit>;
        static auto keyValue(const Key& key)
        {
            if constexpr (text_key)
                return key;
            else
                return NumericStorage<Key>(key);
        }
        static Key keyValue(const KeyControl& control)
        {
            if constexpr (text_key)
                return control.value();
            else
                return static_cast<Key>(std::get<NumericStorage<Key>>(control.value()));
        }
        struct MappedAccess final
        {
            Access field;
            Key key;
            auto operator()(auto& component) const noexcept
                requires mapping
            {
                auto* value = field(component);
                using Item = std::remove_reference_t<decltype(value->begin()->second)>;
                using Result =
                    std::conditional_t<std::is_const_v<std::remove_pointer_t<decltype(value)>>, const Item, Item>;
                if (!value)
                    return static_cast<Result*>(nullptr);
                const auto found = value->find(key);
                return found == value->end() ? nullptr : std::addressof(found->second);
            }
        };
        class Row final : public lux::ui::Element
        {
        public:
            Row(TAssociativeFieldElement& owner, Key key, std::size_t index, EditorResult<void>& status)
                : lux::ui::Element(owner.layout_, lux::ui::ElementId{"row/" + std::to_string(index)}), owner_(owner),
                  key_(std::move(key)), layout_(*this, lux::ui::ElementId{"row"}),
                  key_row_(layout_, lux::ui::ElementId{"key-row"}, lux::ui::ELayoutType::HORIZONTAL),
                  key_control_(key_row_, lux::ui::ElementId{"key"}, keyValue(key_)),
                  remove_(key_row_, lux::ui::ElementId{"remove"}, "Remove")
            {
                this->setStretch({1, 0});
                key_control_.setEnabled(!owner.read_only_);
                remove_.setEnabled(!owner.read_only_);
                if constexpr (mapping)
                    value_ = ItemFactory::create(
                        layout_,
                        lux::ui::ElementId{std::string(owner.id().name()) + "/value/" + std::to_string(index)},
                        owner.editing_,
                        owner.target_,
                        owner.interaction_,
                        status,
                        "Value",
                        owner.read_only_,
                        MappedAccess{owner.access_, key_}
                    );
                connections_[0] = takeConnection(
                    lux::object::LuxObject::connect(
                        std::addressof(key_control_),
                        &KeyControl::edited,
                        [this](lux::ui::EditResult change) noexcept {
                            if (change.began)
                                version_ = owner_.version();
                            if (change.committed)
                            {
                                const auto next = keyValue(key_control_);
                                if (next != key_)
                                    owner_.queue(
                                        [old = key_, next](auto& values) {
                                            const auto found = values.find(old);
                                            if (found == values.end() || values.contains(next))
                                                return false;
                                            if constexpr (mapping)
                                            {
                                                auto value = std::move(found->second);
                                                values.erase(found);
                                                values.emplace(next, std::move(value));
                                            }
                                            else
                                            {
                                                values.erase(found);
                                                values.insert(next);
                                            }
                                            return true;
                                        },
                                        version_
                                    );
                            }
                        }
                    ),
                    status
                );
                connections_[1] = takeConnection(
                    lux::object::LuxObject::connect(
                        std::addressof(remove_),
                        &lux::ui::Button::activated,
                        [this]() noexcept {
                            owner_.queue(
                                [key = key_](auto& values) { return values.erase(key) != 0; },
                                owner_.version()
                            );
                        }
                    ),
                    status
                );
            }
            const Key& key() const noexcept
            {
                return key_;
            }
            bool editing() const noexcept
            {
                return key_control_.editing();
            }

        private:
            void update() noexcept override
            {
                if (!key_control_.editing())
                    key_control_.setValue(keyValue(key_));
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
            TAssociativeFieldElement& owner_;
            Key key_;
            lux::ui::Layout layout_, key_row_;
            KeyControl key_control_;
            lux::ui::Button remove_;
            std::unique_ptr<lux::ui::Element> value_;
            std::array<object::Connection, 2> connections_;
            std::uint64_t version_{};
        };

    public:
        TAssociativeFieldElement(
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
              access_(std::move(access)), label_(std::move(label)), read_only_(read_only),
              layout_(*this, lux::ui::ElementId{"associative"}), title_(layout_, lux::ui::ElementId{"title"}, label_),
              toolbar_(layout_, lux::ui::ElementId{"toolbar"}, lux::ui::ELayoutType::HORIZONTAL),
              previous_(toolbar_, lux::ui::ElementId{"previous"}, "Previous page"),
              next_(toolbar_, lux::ui::ElementId{"next"}, "Next page"),
              insertion_(toolbar_, lux::ui::ElementId{"new-key"}, keyValue(Key{})),
              add_(toolbar_, lux::ui::ElementId{"add"}, "Add key")
        {
            this->setStretch({1, 0});
            connections_[0] = takeConnection(
                lux::object::LuxObject::connect(
                    std::addressof(previous_),
                    &lux::ui::Button::activated,
                    [this]() noexcept { page_request_ = -1; }
                ),
                status
            );
            connections_[1] = takeConnection(
                lux::object::LuxObject::connect(
                    std::addressof(next_),
                    &lux::ui::Button::activated,
                    [this]() noexcept { page_request_ = 1; }
                ),
                status
            );
            connections_[2] = takeConnection(
                lux::object::LuxObject::connect(
                    std::addressof(add_),
                    &lux::ui::Button::activated,
                    [this]() noexcept {
                        queue(
                            [key = keyValue(insertion_)](auto& values) {
                                if constexpr (mapping)
                                    return values.try_emplace(key).second;
                                else
                                    return values.insert(key).second;
                            },
                            version()
                        );
                    }
                ),
                status
            );
            synchronize(true, &status);
        }

    private:
        std::uint64_t version() const noexcept
        {
            return editing_.componentVersion(target_, lux::cxx::typeToken<Component>());
        }
        template <class Mutation> void queue(Mutation mutation, std::uint64_t expected_version)
        {
            if (read_only_)
                return;
            interaction_.queueEdit(
                [editing = &editing_,
                 target = target_,
                 access = access_,
                 expected_version,
                 identity = std::string(this->id().name()),
                 label = label_,
                 mutation = std::move(mutation)]() -> editing::EditResult<void> {
                    if (editing->componentVersion(target, lux::cxx::typeToken<Component>()) != expected_version)
                        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STALE_TARGET));
                    const auto* component =
                        static_cast<const Component*>(editing->component(target, lux::cxx::typeToken<Component>()));
                    const auto* value = component ? access(*component) : nullptr;
                    if (!value)
                        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STALE_TARGET));
                    auto prepared = *value;
                    if (!mutation(prepared))
                        return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED));
                    auto write = editing->writeTarget(target);
                    if (!write)
                        return lux::cxx::unexpected(write.error());
                    auto applied = editing->template setField<Component>(*write, identity, label, access, prepared);
                    if (!applied)
                        return lux::cxx::unexpected(applied.error());
                    return {};
                }
            );
        }
        void synchronize(bool force, EditorResult<void>* initial = nullptr) noexcept
        {
            if (!force && synchronized_ && observed_version_ == version())
                return;
            const auto* component =
                static_cast<const Component*>(editing_.component(target_, lux::cxx::typeToken<Component>()));
            const auto* value = component ? access_(*component) : nullptr;
            const auto count = value ? value->size() : 0;
            first_ = std::min(first_, count ? (count - 1) / page_size * page_size : 0);
            try
            {
                auto& keys = page_keys_;
                keys.clear();
                keys.reserve(std::min(page_size, count - first_));
                if (value)
                    for (auto iterator = std::next(value->begin(), static_cast<std::ptrdiff_t>(first_));
                         iterator != value->end() && keys.size() != page_size;
                         ++iterator)
                    {
                        if constexpr (mapping)
                            keys.push_back(iterator->first);
                        else
                            keys.push_back(*iterator);
                    }
                bool same = keys.size() == rows_.size();
                for (std::size_t i{}; same && i < keys.size(); ++i)
                    same = keys[i] == rows_[i]->key();
                if (!same)
                {
                    if (std::ranges::any_of(rows_, [](const auto& row) { return row->editing(); }) ||
                        !interaction_.finish())
                        return;
                    EditorResult<void> status;
                    std::vector<std::unique_ptr<Row>> candidate;
                    candidate.reserve(keys.size());
                    for (std::size_t i{}; i < keys.size(); ++i)
                        candidate.push_back(std::make_unique<Row>(*this, keys[i], first_ + i, status));
                    if (!status)
                    {
                        if (initial)
                            *initial = status;
                        interaction_.fail("Could not connect the key controls.");
                        return;
                    }
                    rows_ = std::move(candidate);
                }
                observed_version_ = version();
                synchronized_ = true;
                previous_.setEnabled(first_ != 0);
                next_.setEnabled(first_ + page_size < count);
                insertion_.setEnabled(!read_only_);
                add_.setEnabled(!read_only_);
            }
            catch (const std::bad_alloc&)
            {
                std::terminate();
            }
            catch (...)
            {
                if (initial)
                    *initial = lux::cxx::unexpected(EditorFailure{EEditorError::FRONTEND_FAILURE, "inspector.create"});
                interaction_.fail("Could not construct the key controls.");
            }
        }
        void update() noexcept override
        {
            if (page_request_)
            {
                if (std::ranges::any_of(rows_, [](const auto& row) { return row->editing(); }) ||
                    !interaction_.finish() || !interaction_.finishPending())
                    return;
                first_ = std::exchange(page_request_, 0) < 0 ? (first_ >= page_size ? first_ - page_size : 0)
                                                             : first_ + page_size;
                synchronized_ = false;
            }
            synchronize(false);
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
        std::string label_;
        bool read_only_;
        lux::ui::Layout layout_;
        lux::ui::Label title_;
        lux::ui::Layout toolbar_;
        lux::ui::Button previous_, next_;
        KeyControl insertion_;
        lux::ui::Button add_;
        std::vector<std::unique_ptr<Row>> rows_;
        std::vector<Key> page_keys_;
        std::array<object::Connection, 3> connections_;
        std::size_t first_{};
        std::uint64_t observed_version_{};
        int page_request_{};
        bool synchronized_{};
    };
}
