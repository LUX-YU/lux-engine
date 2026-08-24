#pragma once

#include <lux/engine/ecs/System.hpp>
#include <lux/engine/object/Object.hpp>
#include <lux/engine/object/ObjectAnnotations.hpp>
#include <lux/engine/object/ObjectEvent.hpp>

#include <cstdint>
#include <utility>
#include <vector>

namespace lux::ecs::test
{
    struct SectionDemandChanged final
    {
        std::uint32_t section{};
    };

    struct SectionReady final
    {
        Entity entity{NullEntity};
        std::uint32_t section{};
    };

    struct SectionResident final
    {
        std::uint32_t section{};
    };

    class LUX_OBJECT() StreamingDemandSystem final
        : public lux::object::Object<StreamingDemandSystem>,
          public System
    {
      public:
        using Object::Object;

        static const signal_type<SectionDemandChanged> demandChanged;

        void update(const SystemFrame& frame) noexcept override
        {
            if (!demand_published_)
            {
                demand_published_ = true;
                SectionDemandChanged demand{7u};
                notify<demandChanged>(demand);
            }

            for (const SectionReady& ready : inbox_)
            {
                struct MakeResident final
                {
                    Entity entity{NullEntity};
                    std::uint32_t section{};

                    void apply(WorldEdit& edit) noexcept
                    {
                        edit.emplace<SectionResident>(entity, section);
                    }
                };
                if (frame.commands().push(
                    MakeResident{ready.entity, ready.section}
                ) != ECommandResult::ACCEPTED)
                {
                    ++discarded_completions_;
                }
            }
            inbox_.clear();
        }

        [[nodiscard]] bool removable() const noexcept override
        {
            return true;
        }

      protected:
        void event(lux::object::EventView& view) noexcept override
        {
            if (const auto* ready = view.getIf<SectionReady>())
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
        std::vector<SectionReady> inbox_;
        std::uint64_t discarded_completions_{};
        bool demand_published_{};
    };
} // namespace lux::ecs::test
