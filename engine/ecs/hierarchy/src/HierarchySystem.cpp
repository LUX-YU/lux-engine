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

    void HierarchySystem::update(SystemContext& context) noexcept
    {
        impl_->hierarchy->synchronize(context);
    }
} // namespace lux::ecs
