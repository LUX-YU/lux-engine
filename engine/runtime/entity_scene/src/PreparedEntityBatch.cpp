#include <lux/engine/runtime/entity_scene/PreparedEntityBatch.hpp>

#include "EntityBatchInternal.hpp"

#include <utility>

namespace lux::runtime::entity_scene
{
    PreparedEntityBatch::PreparedEntityBatch(
        std::unique_ptr<detail::PreparedEntityBatchImpl> impl) noexcept
        : impl_(std::move(impl))
    {}

    PreparedEntityBatch::~PreparedEntityBatch() = default;
    PreparedEntityBatch::PreparedEntityBatch(PreparedEntityBatch&&) noexcept =
        default;
    PreparedEntityBatch& PreparedEntityBatch::operator=(
        PreparedEntityBatch&&) noexcept = default;

    const lux::ecs::scene_format::EntitySectionId& PreparedEntityBatch::section()
        const noexcept
    {
        static const lux::ecs::scene_format::EntitySectionId empty;
        return impl_ ? impl_->decoded.section() : empty;
    }

    std::uint64_t PreparedEntityBatch::generation() const noexcept
    {
        return impl_ ? impl_->decoded.generation() : 0u;
    }

    std::size_t PreparedEntityBatch::entityCount() const noexcept
    {
        return impl_ ? impl_->decoded.entityCount() : 0u;
    }

    EPreparedEntityBatchState PreparedEntityBatch::state() const noexcept
    {
        return impl_ ? impl_->state : EPreparedEntityBatchState::FAILED;
    }
}
