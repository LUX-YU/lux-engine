#include <lux/engine/ecs/HierarchySystem.hpp>

#include <lux/engine/ecs/Parent.hpp>

namespace lux::ecs
{
    struct HierarchySystem::Impl final
    {
        explicit Impl(HierarchyIndex& value) noexcept
            : hierarchy(std::addressof(value))
        {
        }

        HierarchyIndex* hierarchy{};
    };

    HierarchySystem::HierarchySystem(HierarchyIndex& hierarchy)
        : impl_(std::make_unique<Impl>(hierarchy))
    {
    }

    HierarchySystem::~HierarchySystem() = default;

    SystemAccess HierarchySystem::access() const noexcept
    {
        return lux::ecs::access(
            query<Read<Parent>>(),
            ExternalWrite<HierarchyIndex>{}
        );
    }

    lux::cxx::expected<void, SystemStartError> HierarchySystem::start(
        SystemStart& start
    ) noexcept
    {
        if (!impl_->hierarchy->canStart(start))
        {
            return lux::cxx::unexpected(
                SystemStartError{ESystemStartError::REJECTED}
            );
        }
        return {};
    }

    void HierarchySystem::update(SystemFrame& frame) noexcept
    {
        impl_->hierarchy->synchronize(frame);
    }
} // namespace lux::ecs
