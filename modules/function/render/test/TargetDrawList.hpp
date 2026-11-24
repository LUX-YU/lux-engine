#pragma once

#include "Support.hpp"
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/resource/StorageMeta.hpp>

namespace foundation_test
{
    struct TargetReadProbe final
    {
        static inline std::atomic<nri::Fence*> gate{};
        static inline nri::FenceSubmitDesc first_read{};
        struct Images final
        {
            std::atomic_uint created{}, destroyed{};
            std::array<std::atomic<const nri::Texture*>, 4> slots{};
        };
        static inline std::atomic_uint draws{}, submissions{};
        static inline std::array<Images, 2> banks;
        static inline std::atomic<Images*> capturing{};

        static void attach(DeviceSession& device) noexcept
        {
            if (submit_) return;
            auto& core = const_cast<nri::CoreInterface&>(device.nriCore());
            view_ = core.CreateTextureView;
            destroy_ = core.DestroyTexture;
            draw_ = core.CmdDraw;
            submit_ = core.QueueSubmit;
            core.CreateTextureView = [](const nri::TextureViewDesc& desc, nri::Descriptor*& view) noexcept
            {
                const auto result = view_(desc, view);
                auto* bank = capturing.load();
                if (bank && result == nri::Result::SUCCESS && desc.type == nri::TextureView::COLOR_ATTACHMENT)
                {
                    const auto i = bank->created.fetch_add(1);
                    CHECK(i < bank->slots.size());
                    bank->slots[i] = desc.texture;
                }
                return result;
            };
            core.DestroyTexture = [](nri::Texture* image) noexcept
            {
                for (auto& bank : banks)
                {
                    if (bank.destroyed == bank.created) continue;
                    for (auto& slot : bank.slots)
                    {
                        const nri::Texture* tracked = image;
                        if (tracked && slot.compare_exchange_strong(tracked, nullptr)) ++bank.destroyed;
                    }
                }
                destroy_(image);
            };
            core.CmdDraw = [](nri::CommandBuffer& command, const nri::DrawDesc& draw) noexcept
            {
                if (gate)
                {
                    const auto n = draws.fetch_add(1);
                    CHECK(n < draw_buffers_.size());
                    draw_buffers_[n] = &command;
                }
                draw_(command, draw);
            };
            core.QueueSubmit = [](nri::Queue& queue, const nri::QueueSubmitDesc& desc) noexcept
            {
                auto* held_fence = gate.load();
                if (!held_fence) return submit_(queue, desc);
                const auto commands = std::span{desc.commandBuffers, desc.commandBufferNum};
                const auto count = std::ranges::count_if(draw_buffers_, [&](const auto* draw)
                {
                    return draw && std::ranges::find(commands, draw) != commands.end();
                });
                if (!count) return submit_(queue, desc);
                CHECK(count == 1 && desc.signalFenceNum == 1);
                if (++submissions == 2) return nri::Result::INVALID_ARGUMENT;
                std::vector<nri::FenceSubmitDesc> waits;
                if (desc.waitFenceNum) waits.assign(desc.waitFences, desc.waitFences + desc.waitFenceNum);
                waits.push_back({held_fence, 1, nri::StageBits::ALL});
                auto held = desc;
                held.waitFences = waits.data();
                held.waitFenceNum = static_cast<std::uint32_t>(waits.size());
                first_read = desc.signalFences[0];
                return submit_(queue, held);
            };
        }

    private:
        static inline std::array<const nri::CommandBuffer*, 2> draw_buffers_{};
        static inline decltype(nri::CoreInterface::CreateTextureView) view_{};
        static inline decltype(nri::CoreInterface::DestroyTexture) destroy_{};
        static inline decltype(nri::CoreInterface::CmdDraw) draw_{};
        static inline decltype(nri::CoreInterface::QueueSubmit) submit_{};
    };

    class TargetDrawList final
    {
    public:
        using Config = void;
        using Descriptor = TargetDrawList;
        using PerFrameData = std::tuple<RasterTargetImage>;
        static inline const ResourceDefinition descriptor{
            ResourceNameView{"test.target_draw_list"}, {}, {}, {}, "Two raster batches sampling one publication.",
            EResourceDomain::JOB, resourceFrameTypes(static_cast<PerFrameData*>(nullptr)),
            EResourceContentLifetime::PER_RUN
        };
        static inline DeviceSession* session = nullptr;

        static ResourceRegistration factory() noexcept
        {
            static const StorageBindingOps ops{
                .selectRun = [](void* object, detail::ReplicaFrame frame) noexcept
                {
                    auto& owner = *static_cast<TargetDrawList*>(object);
                    owner.slots_[frame.slot] = std::exchange(owner.pending_, std::nullopt);
                },
                .readDrawList = [](const void* object, std::optional<std::uint32_t> slot) noexcept
                {
                    const auto& owner = *static_cast<const TargetDrawList*>(object);
                    const auto& binding = slot ? owner.slots_[*slot] : owner.pending_;
                    if (!binding) return RasterStreamData{};
                    static const RasterStreamDraw draw{{kGraphInvalidIndex, 0}, {0, 2}, {}, {}, DrawVertices{12}};
                    return RasterStreamData{{&draw, 1}, *binding, {}, {}};
                }
            };
            return makeResourceRegistration<Descriptor, TargetDrawList, &create, &TargetDrawList::update>(&ops);
        }

    private:
        static RenderResult<std::unique_ptr<TargetDrawList>>
        create(ResourceCreateContext& context, ResourceInstanceId) noexcept
        {
            session = &context.device;
            TargetReadProbe::attach(context.device);
            return std::make_unique<TargetDrawList>();
        }

        RenderResult<void> update(std::span<const RasterTargetImage> images) noexcept
        {
            if (images.size() != 2) return lux::cxx::unexpected(RenderError{kFrameContract});
            pending_ = Bindings{RasterStreamBinding{0, images[0], 0}, RasterStreamBinding{0, images[1], 1}};
            return {};
        }

        using Bindings = std::array<RasterStreamBinding, 2>;
        std::optional<Bindings> pending_;
        std::array<std::optional<Bindings>, 3> slots_;
    };
}
