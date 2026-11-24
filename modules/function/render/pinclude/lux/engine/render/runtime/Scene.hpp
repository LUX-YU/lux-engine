#pragma once

#include <lux/cxx/container/StableSlotMap.hpp>
#include <lux/engine/render/runtime/JobId.hpp>
#include <lux/engine/render/resource/view/SceneViewStorage.hpp>
#include <lux/engine/render/runtime/SceneId.hpp>
#include <lux/engine/render/device/DeviceId.hpp>
#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace lux::render::runtime_detail
{
    struct RenderView final
    {
        detail::SceneViewRow parameters;
        std::vector<RenderJobId> jobs;
    };

    struct DeviceViewUse final
    {
        DeviceSessionId device;
        detail::SubmissionFrontiers accepted;
    };

    class RenderScene final
    {
    public:
        [[nodiscard]] RenderView* findView(ViewId id) noexcept
        {
            auto* slot = views_.find(id);
            return slot && *slot ? &**slot : nullptr;
        }

        [[nodiscard]] bool hasJobs() const noexcept
        {
            return std::ranges::any_of(views_, [](const auto& view) { return view && !view->jobs.empty(); });
        }

        [[nodiscard]] ViewId createView(auto&& completed) noexcept
        {
            std::erase_if(retired_, [&](const Retired& row)
            {
                if (!std::ranges::all_of(row.devices, completed)) return false;
                views_.erase(row.view);
                return true;
            });
            return views_.emplace(std::in_place);
        }

        void removeView(ViewId id, std::vector<DeviceViewUse> pending) noexcept
        {
            if (pending.empty()) views_.erase(id);
            else
            {
                // The public identity expires immediately; the sparse slot stays occupied while its GPU row is in use.
                views_.find(id)->reset();
                retired_.push_back({id, std::move(pending)});
            }
        }

    private:
        struct Retired final
        {
            ViewId view;
            std::vector<DeviceViewUse> devices;
        };
        cxx::StableSlotMap<std::optional<RenderView>, ViewTag> views_;
        std::vector<Retired> retired_;
    };
} // namespace lux::render::runtime_detail
