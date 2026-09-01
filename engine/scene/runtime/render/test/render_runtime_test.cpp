#include <lux/engine/scene/RenderRuntime.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.hpp>
#include <lux/engine/scene/Builtin3DRenderIntegration.hpp>
#include <lux/engine/scene/SceneSystem.hpp>

#include <cassert>
#include <cstdlib>
#include <type_traits>
#include <utility>

namespace
{
    class FakeRuntime final : public lux::scene::RenderRuntime
    {
    public:
        [[nodiscard]] lux::cxx::expected<lux::scene::RenderRuntimeLease, lux::scene::RenderRuntimeFailure>
        acquire() noexcept override
        {
            if (stopping)
            {
                return lux::cxx::unexpected(lux::scene::RenderRuntimeFailure{
                    lux::scene::ERenderRuntimeError::STOPPING
                });
            }
            ++demand;
            return makeLease();
        }

        std::size_t demand{};
        std::size_t releases{};
        bool stopping{};

    private:
        void release() noexcept override
        {
            assert(demand != 0U);
            --demand;
            ++releases;
        }

        [[noreturn]] static void unavailable() noexcept
        {
            std::abort();
        }

        lux::render::RenderControlSession& control() noexcept override
        {
            unavailable();
        }

        lux::render::RenderProgramSession& programs() noexcept override
        {
            unavailable();
        }

        lux::render::RenderUploadClient upload() noexcept override
        {
            unavailable();
        }

        const lux::render::FeatureCatalog& features() const noexcept override
        {
            unavailable();
        }
    };
}

int main()
{
    using namespace lux;

    static_assert(!std::is_copy_constructible_v<scene::RenderRuntimeLease>);
    static_assert(std::is_nothrow_move_constructible_v<scene::RenderRuntimeLease>);
    static_assert(scene::SceneSystem<scene::RenderSystem>);

    FakeRuntime runtime;
    {
        auto acquired = runtime.acquire();
        assert(acquired && runtime.demand == 1U);
        scene::RenderRuntimeLease first = std::move(*acquired);
        assert(first);
        scene::RenderRuntimeLease second = std::move(first);
        assert(!first && second);
        scene::RenderRuntimeLease third;
        third = std::move(second);
        assert(!second && third);
    }
    assert(runtime.demand == 0U && runtime.releases == 1U);
    runtime.stopping = true;
    assert(!runtime.acquire());

    const auto registration = scene::builtinRenderSystemRegistration();
    assert(registration.type == system::systemTypeId(scene::RenderSystem::Description.canonical_name));
    assert(registration.configuration.valid());
    assert(registration.requirements.size() == 1U);
    assert(registration.requirements.front().capability == "lux.render.runtime");
    assert(registration.description->multiplicity == system::ESystemMultiplicity::SINGLE_PER_OWNER);
    assert(scene::builtinRenderSystemRegistrations().size() == 1U);
    assert(scene::builtinRenderFeatureSceneBindings().size() == 2U);

    scene::RenderSystemConfiguration configuration;
    configuration.coordinate_page_size = 2048.0;
    configuration.features.push_back({render::featureId("lux.render.mesh_stack.v1"), {}});
    std::vector<std::byte> encoded;
    auto encode_result = registration.configuration.encode(&configuration, encoded);
    assert(encode_result);
    scene::RenderSystemConfiguration decoded;
    assert(registration.configuration.decode(encoded, &decoded));
    assert(decoded.coordinate_page_size == 2048.0);
    assert(decoded.features.size() == 1U && decoded.features.front().type == configuration.features.front().type);
    return 0;
}
