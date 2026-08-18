#include <lux/engine/extensions/physics2d/Physics2D.hpp>

#include <lux/engine/ecs/physics2d/systems/Physics2DSystem.hpp>
#include <lux/engine/extensions/ExtensionAbi.hpp>
#include <lux/engine/runtime/extensions/RuntimeContributionRegistrar.hpp>
#include <lux/engine/runtime/extensions/SceneContributions.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
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

    class Physics2DContributionSystem final : public lux::ecs::ISystem
    {
    public:
        Physics2DContributionSystem(
            Physics2DContributionConfig config)
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

    lux::runtime::SceneContributionDescriptor makeContribution()
    {
        Physics2DContributionConfig defaults;
        const auto default_bytes = lux::cxx::SharedBytes<>::copyOf(
            std::as_bytes(std::span{&defaults, 1u}));

        lux::runtime::SceneContributionDescriptor descriptor;
        descriptor.id = lux::extensions::ContributionId{
            "org.lux.physics2d.world"};
        descriptor.display_name = "Physics 2D";
        descriptor.provided_services = {
            lux::ecs::typeToken<PhysicsWorldApi>()};
        descriptor.config_schema_version = 1u;
        descriptor.default_config = lux::runtime::ContributionConfig{
            1u, default_bytes};
        descriptor.provider = lux::extensions::ExtensionId{
            "org.lux.physics2d"};
        descriptor.build = [](
            lux::runtime::SceneContributionBatchBuilder& builder,
            const lux::runtime::SceneContributionBuildContext&,
            lux::runtime::ContributionConfig config)
            -> lux::cxx::expected<
                void, lux::runtime::SceneContributionBuildFailure>
        {
            if (config.schema_version != 1u ||
                config.bytes.size() != sizeof(Physics2DContributionConfig))
            {
                return lux::cxx::unexpected(
                    lux::runtime::SceneContributionBuildFailure{
                        lux::runtime::ESceneContributionBuildError::INVALID_CONFIG});
            }
            Physics2DContributionConfig decoded;
            std::memcpy(
                &decoded,
                config.bytes.data(),
                sizeof(decoded));
            auto system = std::make_unique<Physics2DContributionSystem>(
                decoded);
            auto capability = builder.publishService(
                system->makeApi());
            if (!capability)
                return lux::cxx::unexpected(capability.error());
            return builder.add(
                std::move(system),
                lux::ecs::kPhaseSimulation);
        };
        return descriptor;
    }

} // namespace lux::extensions::physics2d

extern "C" LUX_PHYSICS2D_EXTENSION_PUBLIC
const lux::extensions::ExtensionModuleDescriptorV4*
luxGetExtensionModuleV4() noexcept
{
    using namespace lux::extensions;
    static constexpr ExtensionModuleDescriptorV4 descriptor{
        sizeof(ExtensionModuleDescriptorV4),
        kExtensionAbiV4,
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
luxRegisterRuntimeContributionsV4(
    lux::extensions::RuntimeContributionRegistrar& registrar) noexcept
{
    const auto added = registrar.sceneContributions().add(
        lux::extensions::physics2d::makeContribution());
    return added
        ? lux::extensions::ExtensionRegistrationResult{}
        : lux::extensions::ExtensionRegistrationResult{
              lux::extensions::EExtensionRegistrationError::
                  INVALID_DESCRIPTOR};
}
