#include <cstring>
#include <lux/engine/scene/render/DirectLightValues.hpp>
#include <lux/engine/scene/render/LitSceneInputOperation.hpp>

namespace lux::scene
{
    using namespace lux::render;

    std::span<const RenderPayloadContract> LitSceneInputOperation::inputs() noexcept
    {
        static const auto contracts = []
        {
            RenderPayloadContract value;
            value.schema = kSceneGeometryInput;
            value.use = ERenderPayloadUse::EFFECT_INPUT;
            value.lifetime = ERenderPayloadLifetime::MOVED_VALUE;
            value.setCppType(cxx::typeToken<LitSceneInputs>());
            return std::array{value};
        }();
        return contracts;
    }

    RenderResult<std::unique_ptr<LitSceneInputOperation>> LitSceneInputOperation::create(RenderBindingContext& context
    ) noexcept
    {
        std::vector<MeshRenderBinding*> meshes;
        context.forEach<MeshRenderBinding>([&](MeshRenderBinding& binding) noexcept { meshes.push_back(&binding); });
        if (meshes.empty())
        {
            return cxx::unexpected(RenderError{kRuntimeUnsupported});
        }
        return std::unique_ptr<LitSceneInputOperation>{new LitSceneInputOperation(context, std::move(meshes))};
    }

    std::uint64_t LitSceneInputOperation::effectRevision() noexcept
    {
        if (cached_revision_ != revision_)
        {
            std::vector<RasterGeometryInput> geometry;
            std::uint32_t capacity = 0;
            for (const auto* meshes : meshes_)
            {
                auto role = meshes->geometry();
                geometry
                    .insert(geometry.end(), std::make_move_iterator(role.begin()), std::make_move_iterator(role.end()));
                capacity += meshes->instanceCapacity();
            }
            std::ranges::sort(
                geometry,
                [](const auto& a, const auto& b) noexcept { return a.selection < b.selection; }
            );
            const auto repeated = std::ranges::unique(geometry, {}, &RasterGeometryInput::selection);
            geometry.erase(repeated.begin(), repeated.end());
            capacity = std::max(capacity, inputs_.instance_capacity);
            std::vector<SceneLightInput> lights;
            for (const auto entity : registry_.view<const detail::LightRenderResourceComponent>())
            {
                const auto& resource = registry_.get<const detail::LightRenderResourceComponent>(entity);
                if (resource.shadows.enabled)
                {
                    lights.push_back({resource.light, detail::compiledLightDescription(resource)});
                }
            }
            std::ranges::sort(
                lights,
                [](const auto& left, const auto& right) noexcept { return left.light < right.light; }
            );
            const bool same_geometry =
                geometry.size() == inputs_.geometry.size() &&
                std::equal(
                    geometry.begin(),
                    geometry.end(),
                    inputs_.geometry.begin(),
                    [](const auto& left, const auto& right) noexcept { return left.selection == right.selection; }
                );
            if (!same_geometry || lights != inputs_.lights || capacity != inputs_.instance_capacity)
            {
                ++effect_revision_;
            }
            inputs_.geometry = std::move(geometry);
            inputs_.lights = std::move(lights);
            inputs_.instance_capacity = capacity;
            cached_revision_ = revision_;
        }
        return effect_revision_;
    }

    RenderResult<ConfigValue> LitSceneInputOperation::effectInputs(
        cxx::TypeToken type,
        std::span<const std::byte> bytes
    ) noexcept
    {
        if (type != cxx::typeToken<CameraInput>() || bytes.size() != sizeof(CameraInput))
        {
            return cxx::unexpected(RenderError{kCommandContract});
        }
        static_cast<void>(effectRevision());
        CameraInput camera;
        std::memcpy(&camera, bytes.data(), sizeof(camera));
        inputs_.near_distance = camera.near_distance;
        inputs_.far_distance = camera.far_distance;
        return ConfigValue::make(inputs_);
    }
} // namespace lux::scene
