#pragma once

#include <lux/engine/simulation/ecs/EcsCommandBuffer.hpp>
#include <lux/engine/simulation/scripting/ScriptBackend.hpp>

namespace lux::simulation::script
{
    struct ScriptDeferredComponent final
    {
        ScriptHostComponentContract contract;
        const void* (*read)(const ecs::Registry&, ecs::Entity) noexcept{};
        bool (*replace)(ecs::EcsCommandWriter&, ecs::Entity, const void*) noexcept{};
        bool (*emplace)(ecs::EcsCommandWriter&, ecs::Entity, const void*) noexcept{};
        bool (*remove)(ecs::EcsCommandWriter&, ecs::Entity) noexcept{};
    };

    // Native composition explicitly opts a component into the script surface. No runtime reflection or raw memcpy.
    template <class Component> [[nodiscard]] ScriptDeferredComponent
    scriptDeferredComponent(ScriptHostComponentContract contract) noexcept
    {
        static_assert(std::is_nothrow_copy_constructible_v<Component>);
        static_assert(std::is_nothrow_move_constructible_v<Component>);
        static_assert(std::is_nothrow_move_assignable_v<Component>);
        static_assert(std::is_nothrow_destructible_v<Component>);
        contract.size = sizeof(Component);
        contract.alignment = alignof(Component);
        return {contract,
            [](const ecs::Registry& registry, ecs::Entity entity) noexcept -> const void* {
                return registry.template try_get<Component>(entity);
            },
            [](ecs::EcsCommandWriter& writer, ecs::Entity entity, const void* value) noexcept {
                Component owned(*static_cast<const Component*>(value));
                return writer.canRecord(sizeof(Component), alignof(Component)) &&
                    writer.template replace<Component>(entity, std::move(owned));
            },
            [](ecs::EcsCommandWriter& writer, ecs::Entity entity, const void* value) noexcept {
                using Payload = std::tuple<Component>;
                Component owned(*static_cast<const Component*>(value));
                return writer.canRecord(sizeof(Payload), alignof(Payload)) &&
                    writer.template emplace<Component>(entity, std::move(owned));
            },
            [](ecs::EcsCommandWriter& writer, ecs::Entity entity) noexcept {
                return writer.canRecord() && writer.template remove<Component>(entity);
            }
        };
    }

    // Borrows the native owner's Registry and cold component catalogue. The only mutable script access is a writer
    // borrowed for one explicit execution region. End that writer before the owner's existing command barrier.
    class DeferredScriptHost final
    {
    public:
        class Batch final
        {
        public:
            Batch(const Batch&) = delete;
            Batch& operator=(const Batch&) = delete;
            Batch(Batch&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)) {}
            ~Batch() noexcept { if (owner_ != nullptr) owner_->writer_ = nullptr; }
        private:
            friend class DeferredScriptHost;
            explicit Batch(DeferredScriptHost& owner) noexcept : owner_(&owner) {}
            DeferredScriptHost* owner_{};
        };

        DeferredScriptHost(ecs::Registry& registry, std::span<const ScriptDeferredComponent> components) noexcept
            : registry_(registry), components_(components) {}
        DeferredScriptHost(const DeferredScriptHost&) = delete;
        DeferredScriptHost& operator=(const DeferredScriptHost&) = delete;

        [[nodiscard]] std::optional<Batch> begin(ecs::EcsCommandWriter& writer) noexcept
        {
            if (writer_ != nullptr || !writer || writer.policy() != ecs::EEcsCommandPolicy::CONTINUE_ON_INVALID_TARGET)
                return std::nullopt;
            writer_ = &writer;
            return Batch{*this};
        }

        [[nodiscard]] ScriptHostApi api() noexcept
        {
            return {this,
                [](void* context, ecs::Entity entity, std::uint64_t type) noexcept -> const void* {
                    auto& owner = *static_cast<DeferredScriptHost*>(context);
                    const auto* component = owner.find(type);
                    return component && owner.registry_.valid(entity) ? component->read(owner.registry_, entity) :
                        nullptr;
                },
                [](void* context, ecs::Entity entity, std::uint64_t type, const void* value) noexcept {
                    auto& owner = *static_cast<DeferredScriptHost*>(context);
                    const auto* component = owner.find(type);
                    const bool valid = owner.writer_ && component && value && owner.registry_.valid(entity);
                    const bool accepted = valid && component->replace(*owner.writer_, entity, value);
                    owner.record(accepted);
                    return accepted;
                },
                [](void* context, EScriptHostCommand command, ecs::Entity entity,
                    std::uint64_t type, const void* value) noexcept {
                    auto& owner = *static_cast<DeferredScriptHost*>(context);
                    const bool accepted = owner.command(command, entity, type, value);
                    owner.record(accepted);
                    return accepted;
                },
                [](void* context, std::uint64_t type, ScriptHostComponentContract& result) noexcept {
                    const auto* component = static_cast<DeferredScriptHost*>(context)->find(type);
                    if (!component) return false;
                    result = component->contract;
                    return true;
                }
            };
        }

        [[nodiscard]] std::uint64_t accepted() const noexcept { return accepted_; }
        [[nodiscard]] std::uint64_t rejected() const noexcept { return rejected_; }

    private:
        [[nodiscard]] const ScriptDeferredComponent* find(std::uint64_t type) const noexcept
        {
            for (const auto& component : components_)
                if (component.contract.component_type == type) return &component;
            return nullptr;
        }
        void record(bool accepted) noexcept
        {
            if (accepted) ++accepted_;
            else ++rejected_;
        }
        [[nodiscard]] bool command(EScriptHostCommand command, ecs::Entity entity,
            std::uint64_t type, const void* value) noexcept
        {
            if (writer_ == nullptr) return false;
            if (command == EScriptHostCommand::CREATE_ENTITY)
                return writer_->canRecord() && writer_->create().valid();
            if (!registry_.valid(entity)) return false;
            if (command == EScriptHostCommand::DESTROY_ENTITY)
            {
                if (!writer_->canRecord()) return false;
                writer_->destroy(entity);
                return true;
            }
            const auto* component = find(type);
            if (!component) return false;
            if (command == EScriptHostCommand::EMPLACE_COMPONENT)
                return value && component->emplace(*writer_, entity, value);
            if (command == EScriptHostCommand::REMOVE_COMPONENT)
                return component->remove(*writer_, entity);
            return false;
        }

        ecs::Registry& registry_;
        std::span<const ScriptDeferredComponent> components_;
        ecs::EcsCommandWriter* writer_{};
        std::uint64_t accepted_{};
        std::uint64_t rejected_{};
    };
}
