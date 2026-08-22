#include <lux/engine/extensions/physics2d/Physics2D.hpp>

#include <lux/engine/ecs/ScheduleBuilder.hpp>
#include <lux/engine/ecs/physics2d/systems/Physics2DSystem.hpp>
#include <lux/engine/extensions/ExtensionAbi.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>

namespace lux::extensions::physics2d
{
    struct PhysicsWorldApi::State final
    {
        std::atomic<std::uint64_t> completed_steps{0u};
    };

    PhysicsWorldApi::PhysicsWorldApi(std::shared_ptr<State> state) noexcept
        : state_(std::move(state))
    {}

    std::uint64_t PhysicsWorldApi::completedSteps() const noexcept
    {
        return state_
            ? state_->completed_steps.load(std::memory_order_relaxed)
            : 0u;
    }

    class Physics2DExtensionSystem final : public lux::ecs::ISystem
    {
    public:
        Physics2DExtensionSystem(
            Physics2DInstallConfig config)
            : physics_(config.physics)
            , fixed_(config.fixed_step)
            , state_(std::make_shared<PhysicsWorldApi::State>())
        {}

        [[nodiscard]] std::unique_ptr<PhysicsWorldApi> makeApi() const
        {
            return std::unique_ptr<PhysicsWorldApi>(
                new PhysicsWorldApi(state_));
        }

        void update(const lux::ecs::SystemUpdateContext& context) override
        {
            if (!(fixed_.fixed_dt > 0.0f) || fixed_.max_substeps <= 0)
                return;
            const auto dt = std::max(context.dt(), 0.0f);
            accumulated_ = std::min(
                accumulated_ + dt,
                std::max(fixed_.max_accumulated, fixed_.fixed_dt));
            int substeps = 0;
            while (accumulated_ >= fixed_.fixed_dt &&
                   substeps < fixed_.max_substeps)
            {
                physics_.step(context.registry(), fixed_.fixed_dt);
                accumulated_ -= fixed_.fixed_dt;
                ++substeps;
            }
            if (substeps == fixed_.max_substeps &&
                accumulated_ >= fixed_.fixed_dt &&
                fixed_.drop_excess_time)
            {
                accumulated_ = std::fmod(accumulated_, fixed_.fixed_dt);
            }
            state_->completed_steps.fetch_add(
                static_cast<std::uint64_t>(substeps),
                std::memory_order_relaxed);
        }

        [[nodiscard]] bool supportsDynamicRemoval() const noexcept override
        {
            return true;
        }

    private:
        lux::ecs::Physics2DSystem physics_;
        lux::ecs::FixedStepConfig fixed_;
        std::shared_ptr<PhysicsWorldApi::State> state_;
        float accumulated_{0.0f};
    };

    ExtensionRegistrationResult installWorldSystems(
        lux::ecs::ScheduleBuilder& builder) noexcept
    {
        Physics2DInstallConfig defaults;
        auto system = std::make_unique<Physics2DExtensionSystem>(defaults);
        auto api = system->makeApi();
        const auto checkpoint = builder.checkpoint();
        if (!builder.services().emplace(std::move(api)))
        {
            return ExtensionRegistrationResult{
                EExtensionRegistrationError::DUPLICATE_REGISTRATION};
        }
        if (!builder.add(
                std::move(system),
                lux::ecs::kPhaseSimulation))
        {
            (void)builder.rollbackTo(checkpoint);
            return ExtensionRegistrationResult{
                EExtensionRegistrationError::DUPLICATE_REGISTRATION};
        }
        return {};
    }

} // namespace lux::extensions::physics2d

extern "C" LUX_PHYSICS2D_EXTENSION_PUBLIC
const lux::extensions::ExtensionModuleDescriptorV5*
luxGetExtensionModuleV5() noexcept
{
    using namespace lux::extensions;
    static constexpr ExtensionModuleDescriptorV5 descriptor{
        sizeof(ExtensionModuleDescriptorV5),
        kExtensionAbiV5,
        kEngineExtensionAbiFingerprint,
        lux::cxx::AbiStringView{"org.lux.physics2d"},
        ExtensionVersion{1u, 0u, 0u},
        EExtensionModuleTarget::RUNTIME,
        nullptr,
        0u};
    return &descriptor;
}

extern "C" LUX_PHYSICS2D_EXTENSION_PUBLIC
lux::extensions::ExtensionRegistrationResult
luxInstallWorldSystemsV5(
    lux::ecs::ScheduleBuilder& builder) noexcept
{
    return lux::extensions::physics2d::installWorldSystems(builder);
}
