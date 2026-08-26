#pragma once

#include <lux/engine/ecs/EcsTaskAccess.hpp>
#include <lux/engine/object/Object.hpp>
#include <lux/engine/object/ObjectAnnotations.hpp>
#include <lux/engine/object/ObjectEvent.hpp>

#include <cstdint>
#include <utility>
#include <vector>

namespace lux::ecs::test
{
    struct MaterialTextureDemand final
    {
        std::uint32_t material{};
        std::uint32_t texture{};
    };

    struct TextureReady final
    {
        Entity entity{NullEntity};
        std::uint32_t texture{};
    };

    struct MaterialTextureResident final
    {
        std::uint32_t texture{};
    };

    class LUX_OBJECT() MaterialTextureSystem final
        : public lux::object::Object<MaterialTextureSystem>
    {
    public:
        using Object::Object;

        static const signal_type<MaterialTextureDemand> textureDemand;
        // Registry composition metadata; invocation remains domain-owned.
        inline static constexpr auto Access = makeSystemAccessSpec<>();
        inline static constexpr auto TaskAccess = access<>;

        void invokeTask(
            EcsState&,
            WorldChangeBatch&,
            WorldCommands commands
        ) noexcept
        {
            if (!demand_published_)
            {
                demand_published_ = true;
                MaterialTextureDemand demand{3U, 7U};
                notify<textureDemand>(demand);
            }

            for (const TextureReady& ready : inbox_)
            {
                struct MakeResident final
                {
                    Entity entity{NullEntity};
                    std::uint32_t texture{};

                    void apply(EcsMutation& mutation) noexcept
                    {
                        mutation.emplace<MaterialTextureResident>(
                            entity,
                            texture
                        );
                    }
                };
                if (commands.push(
                        MakeResident{ready.entity, ready.texture}
                    ) != ECommandResult::ACCEPTED)
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
                try
                {
                    inbox_.push_back(*ready);
                }
                catch (...)
                {
                    ++discarded_completions_;
                }
                view.accept();
            }
        }

    private:
        std::vector<TextureReady> inbox_;
        std::uint64_t discarded_completions_{};
        bool demand_published_{};
    };
}
