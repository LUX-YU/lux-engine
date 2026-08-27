#pragma once

#include <lux/engine/object/Object.hpp>
#include <lux/engine/object/ObjectAnnotations.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/simulation/SystemAccessSpec.hpp>
#include <lux/engine/simulation/SystemDescription.hpp>
#include <lux/engine/simulation/ecs/EcsCommandBuffer.hpp>

#include <array>
#include <cstdint>
#include <new>
#include <string_view>
#include <vector>

namespace lux::simulation::test
{
    struct MaterialTextureDemand final
    {
        std::uint32_t material{};
        std::uint32_t texture{};
    };

    struct TextureReady final
    {
        ecs::Entity entity{ecs::NullEntity};
        std::uint32_t texture{};
    };

    struct MaterialTextureResident final
    {
        std::uint32_t texture{};
    };

    class LUX_OBJECT() MaterialTextureSystem final : public lux::object::Object<MaterialTextureSystem>
    {
    public:
        using Object::Object;

        static const signal_type<MaterialTextureDemand> textureDemand;
        inline static constexpr std::array Capabilities{std::string_view{"material.texture-residency"}};
        inline static constexpr std::array Hooks{makeSystemHookPoint<void()>("update")};
        inline static constexpr std::array Events{makeSystemEvent<MaterialTextureDemand>(
            "texture-demand",
            Hooks[0],
            ESystemEventTarget::GLOBAL,
            "lux.material.TextureDemand",
            1U)};
        inline static constexpr auto Access = makeSystemAccessSpec<ComponentWrite<MaterialTextureResident>>();
        inline static constexpr SystemDescription Description{
            .canonical_name = "lux.test.material-texture",
            .version = 1U,
            .capabilities = Capabilities,
            .hooks = Hooks,
            .events = Events};

        [[nodiscard]] bool prepare(std::size_t completion_capacity) noexcept
        {
            try
            {
                inbox_.reserve(completion_capacity);
                return true;
            }
            catch (const std::bad_alloc&)
            {
                return false;
            }
        }

        void invokeTask(ecs::EcsCommandWriter& commands) noexcept
        {
            if (!demand_published_)
            {
                demand_published_ = true;
                const MaterialTextureDemand demand{3U, 7U};
                notify<textureDemand>(demand);
            }

            for (const TextureReady& ready : inbox_)
            {
                if (!commands.emplace<MaterialTextureResident>(ready.entity, MaterialTextureResident{ready.texture}))
                {
                    ++discarded_completions_;
                }
            }
            inbox_.clear();
        }

    protected:
        void event(lux::object::EventView& view) noexcept override
        {
            if (const auto* ready = view.getIf<TextureReady>())
            {
                if (inbox_.size() < inbox_.capacity())
                    inbox_.push_back(*ready);
                else
                    ++discarded_completions_;
                view.accept();
            }
        }

    private:
        std::vector<TextureReady> inbox_;
        std::uint64_t discarded_completions_{};
        bool demand_published_{};
    };
}
