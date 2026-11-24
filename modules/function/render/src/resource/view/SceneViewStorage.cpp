#include <lux/engine/render/frame/FrameError.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/ResourceUploader.hpp>
#include <lux/engine/render/resource/StorageMeta.hpp>
#include <lux/engine/render/assembly/BuiltinEffects.hpp>
#include <lux/engine/render/resource/view/SceneViewStorage.hpp>

#include <lux/engine/render/graph/RasterViewport.hpp>
#include <lux/engine/render/resource/StorageBinding.hpp>
#include <lux/engine/render/resource/view/Resource.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace lux::render::detail
{
    // The Scene outlives its device projections. Each bound SRV starts at the row selected by ViewId.
    class SceneViewBinding final
    {
    public:
        SceneViewBinding(SceneViewRow& row, SceneViewStorage& table, std::uint32_t index) noexcept
            : row_(row), table_(table), index_(index) {}
        static RenderResult<std::unique_ptr<SceneViewBinding>>
        create(ResourceCreateContext& context, ResourceInstanceId) noexcept
        {
            if (!context.view_row_ || !context.view_table_) return cxx::unexpected(RenderError{kStorageMismatch});
            return std::make_unique<SceneViewBinding>(*context.view_row_, *context.view_table_, context.view_index_);
        }

        [[nodiscard]] RenderResult<void> update(std::span<const ViewGpuParameters> values) noexcept
        {
            return row_.update(values);
        }

        [[nodiscard]] RenderResult<ResourceViewValue<ViewGpuParameters>> selectParameters() const noexcept
        {
            return ResourceViewValue<ViewGpuParameters>{selected_, selected_revision_};
        }

    private:
        friend ResourceRegistration sceneViewRegistration() noexcept;

        [[nodiscard]] std::uint64_t offset() const noexcept
        {
            return std::uint64_t{index_ % SceneViewStorage::rows_per_page} * sizeof(ViewGpuParameters);
        }

        void select() noexcept
        {
            if (selected_revision_ == row_.revision()) return;
            selected_ = row_.parameters().value.get();
            selected_revision_ = row_.revision();
        }

        ViewGpuParameters selected_;
        std::uint64_t selected_revision_ = 0;
        std::array<std::uint64_t, 3> uploaded_revisions_{};
        SceneViewRow& row_;
        SceneViewStorage& table_;
        std::uint32_t index_;
        SceneViewStorage::Page* page_ = nullptr;
    };

    namespace
    {
        RenderResult<void> validate(const ViewGpuParameters& value) noexcept
        {
            const auto& v = value.viewport;
            if (auto valid = RasterViewport{v[0], v[1], v[2], v[3]}.validate(); !valid) return valid;
            const auto finite = [](const auto& values) noexcept
            {
                return std::ranges::all_of(values, [](float item) noexcept { return std::isfinite(item); });
            };
            const auto& p = value.position_near;
            const auto& d = value.direction_far;
            const auto direction_length_squared = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
            const bool invalid_values = !finite(value.view) || !finite(value.projection) ||
                !finite(value.inverse_view_projection) || !finite(value.background) || !finite(p) || !finite(d);
            const bool invalid_planes = p[3] <= 0 || d[3] <= p[3];
            const bool invalid_direction = std::abs(direction_length_squared - 1.F) > 0.001F;
            if (const bool invalid_view = invalid_values || invalid_planes || invalid_direction; invalid_view)
                return cxx::unexpected(RenderError{kFrameContract});
            return {};
        }

    } // namespace

    RenderResult<void> SceneViewRow::update(std::span<const ViewGpuParameters> values) noexcept
    {
        for (const auto& value : values)
        {
            if (auto valid = validate(value); !valid) return valid;
        }
        if (values.empty() || values.back() == value_) return {};
        if (revision_ == UINT64_MAX) return cxx::unexpected(RenderError{kRuntimeCapacity});
        value_ = values.back();
        ++revision_;
        return {};
    }

    SceneViewStorage::~SceneViewStorage() noexcept
    {
        for (const auto& [index, page] : pages_) retire(page.buffers);
    }

    void SceneViewStorage::retire(const Buffers& buffers) noexcept
    {
        for (auto* buffer : buffers)
        {
            if (buffer)
                device_.deferRelease([buffer, destroy = device_.nriCore().DestroyBuffer]() noexcept { destroy(buffer); });
        }
    }

    void SceneViewStorage::releasePage(std::uint32_t index) noexcept
    {
        const auto found = pages_.find(index);
        if (found == pages_.end()) return;
        retire(found->second.buffers);
        pages_.erase(found);
    }

    SubmissionFrontiers SceneViewStorage::takePendingUses(std::uint32_t view_index) noexcept
    {
        const auto page = pages_.find(view_index / rows_per_page);
        if (page == pages_.end()) return {};
        auto uses = std::exchange(page->second.uses[view_index % rows_per_page], {});
        // A terminal queue may have issued work without returning its serial. Hold the row until device removal.
        if (!uses.size() || (!device_.terminalError().type && device_.reached(uses))) return {};
        return uses;
    }

    RenderResult<SceneViewStorage::Page*> SceneViewStorage::page(std::uint32_t view_index) noexcept
    {
        const auto key = view_index / rows_per_page;
        auto [entry, inserted] = pages_.try_emplace(key);
        auto& page = entry->second;
        if (inserted)
        {
            if (!frames_ || frames_ > page.buffers.size()) std::terminate();
            const nri::BufferDesc desc{
                sizeof(ViewGpuParameters) * rows_per_page, sizeof(ViewGpuParameters), nri::BufferUsageBits::SHADER_RESOURCE
            };
            for (std::uint32_t slot = 0; slot < frames_; ++slot)
            {
                const auto result = device_.nriCore().CreatePlacedBuffer(
                    device_.device(), NriDeviceHeap, desc, page.buffers[slot]
                );
                if (result == nri::Result::SUCCESS) continue;
                for (std::uint32_t previous = 0; previous < slot; ++previous)
                    device_.nriCore().DestroyBuffer(page.buffers[previous]);
                pages_.erase(entry);
                return cxx::unexpected(nativeError(result));
            }
        }
        auto& uses = page.uses[view_index % rows_per_page];
        if (!uses.size()) uses = device_.makeFrontiers();
        return &page;
    }

    ResourceRegistration sceneViewRegistration() noexcept
    {
        static constexpr auto cpu_view =
            ResourceViewOps::make<SceneViewBinding, ViewGpuParameters, &SceneViewBinding::selectParameters>();
        static constexpr StorageBindingOps binding{
            .prepareFrame = [](void* source, ReplicaFrame frame, ResourceUploader& uploader) noexcept -> RenderResult<bool>
            {
                const auto& view = *static_cast<const SceneViewBinding*>(source);
                if (!view.page_ || view.uploaded_revisions_[frame.slot] == view.selected_revision_) return true;
                const auto& value = view.selected_;
                const BufferWrite write{view.page_->buffers[frame.slot], view.offset(), std::as_bytes(std::span{&value, 1})};
                auto staged = uploader.stageBufferPrefix(write, sizeof(ViewGpuParameters));
                if (!staged) return cxx::unexpected(staged.error());
                return *staged == sizeof(ViewGpuParameters);
            },
            .describeGraphImport = [](const void*, std::uint32_t projection) noexcept -> RenderResult<ResourceProjection>
            {
                if (projection) return cxx::unexpected(RenderError{kStorageMismatch});
                return ViewParameterResource::projection();
            },
            .resolveGraphImport = [](void* source, std::uint32_t projection, const VGraphResourceDesc& shape,
                                     std::span<ResourceInput> outputs) noexcept -> RenderResult<void>
            {
                auto& view = *static_cast<SceneViewBinding*>(source);
                const auto* buffer = std::get_if<GraphBufferDesc>(&shape);
                if (projection || !buffer || buffer->size != sizeof(ViewGpuParameters))
                    return cxx::unexpected(RenderError{kStorageMismatch});
                auto page = view.table_.page(view.index_);
                if (!page) return cxx::unexpected(page.error());
                view.page_ = *page;
                if (outputs.size() > view.page_->buffers.size()) return cxx::unexpected(RenderError{kStorageMismatch});
                auto& uses = view.page_->uses[view.index_ % SceneViewStorage::rows_per_page];
                for (std::size_t i = 0; i < outputs.size(); ++i)
                    outputs[i] = {
                        .buffer = view.page_->buffers[i], .offset = view.offset(), .size = sizeof(ViewGpuParameters),
                        .queue_uses = {.accepted = &uses}
                    };
                return {};
            },
            .cpu_view = cpu_view,
            .settleFrame = [](void* source, const ReplicaSettlement& receipt) noexcept
            {
                if (receipt.accepted_copy.serial)
                {
                    auto& view = *static_cast<SceneViewBinding*>(source);
                    view.uploaded_revisions_[receipt.frame.slot] = view.selected_revision_;
                    view.page_->uses[view.index_ % SceneViewStorage::rows_per_page].accept(receipt.accepted_copy);
                }
            },
            .selectRun = [](void* source, ReplicaFrame) noexcept { static_cast<SceneViewBinding*>(source)->select(); }
        };
        return makeResourceRegistration<ViewParameterResource, SceneViewBinding, &SceneViewBinding::create,
                                        &SceneViewBinding::update>(&binding);
    }
} // namespace lux::render::detail
