#include "Shader.hpp"
#include <algorithm>
#include <bit>
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <lux/engine/render/native/vulkan/CompiledGraph.hpp>
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/Error.hpp>
#include <lux/engine/render/native/vulkan/StorageBinding.hpp>
#include <lux/engine/render/native/vulkan/texture/TextureStorage.hpp>

namespace lux::render::vulkan
{
    // Budget accounting follows physical content, including snapshots held after logical deletion.
    // It owns no device, resource or identity table.
    struct TextureStorage::Residency final
    {
        std::size_t versions = 0;
        std::uint64_t bytes = 0, peak = 0;
    };

    TextureStorage::TextureData::TextureData(
        std::shared_ptr<Residency> residency,
        std::unique_ptr<Image> image,
        DescriptorRange descriptor,
        const rdesc::TextureInfo& info,
        std::uint64_t revision
    ) noexcept
        : residency_(std::move(residency)), image_(std::move(image)), descriptor_(std::move(descriptor)), info_(info),
          bytes_(image_->memoryBinding().size), revision_(revision)
    {
        ++residency_->versions;
        residency_->bytes += bytes_;
        residency_->peak = std::max(residency_->peak, residency_->bytes);
    }

    TextureStorage::TextureData::~TextureData() noexcept
    {
        const auto device = image_->device();
        if (view_)
        {
            vkDestroyImageView(device, view_, nullptr);
        }
        if (sampler_)
        {
            sampler_.release(device);
        }
        --residency_->versions;
        residency_->bytes -= bytes_;
    }

    namespace
    {
        struct Format final
        {
            VkFormat native;
            std::uint32_t extent, bytes;
            bool srgb;
        };

        Format format(rdesc::ETexturePixelFormat value) noexcept
        {
            switch (value)
            {
            case rdesc::ETexturePixelFormat::RGBA8_UNORM:
                return {VK_FORMAT_R8G8B8A8_UNORM, 1, 4, false};
            case rdesc::ETexturePixelFormat::RGBA8_SRGB:
                return {VK_FORMAT_R8G8B8A8_SRGB, 1, 4, true};
            case rdesc::ETexturePixelFormat::BC1_SRGB:
                return {VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 4, 8, true};
            default:
                return {};
            }
        }
    } // namespace

    RenderResult<TextureStorage::ImagePreparation> TextureStorage::prepareImage(
        const rdesc::TextureInfo& info,
        std::span<const std::byte> source,
        std::optional<TextureRegion> region
    ) noexcept
    {
        auto& session = session_;
        const auto layout = format(info.pixel_format);
        const auto max_dimension = session.physicalDevice().properties().properties.limits.maxImageDimension2D;
        const bool bad_extent = info.width <= 0 || info.height <= 0 || std::uint32_t(info.width) > max_dimension ||
                                std::uint32_t(info.height) > max_dimension;
        const bool bad_count = info.layers != 1 || info.mip_count == 0 || info.mip_count > rdesc::kTextureMaxMipCount;
        const bool bad_color = info.color_space == rdesc::ETextureColorSpace::UNKNOWN ||
                               info.color_space > rdesc::ETextureColorSpace::DATA ||
                               (layout.srgb != (info.color_space == rdesc::ETextureColorSpace::SRGB));
        const bool premultiplied_srgb =
            layout.srgb && rdesc::hasTextureFlag(info.flags, rdesc::ETextureAssetFlags::PREMULTIPLIED_ALPHA);
        const bool bad_flags =
            (info.flags & ~7u) != 0 ||
            (rdesc::hasTextureFlag(info.flags, rdesc::ETextureAssetFlags::COMPRESSED) != (layout.extent == 4)) ||
            (rdesc::hasTextureFlag(info.flags, rdesc::ETextureAssetFlags::NO_MIPS) && info.mip_count != 1);
        if (layout.native == VK_FORMAT_UNDEFINED)
        {
            return cxx::unexpected(RenderError{kUnsupported});
        }
        if (bad_extent || bad_count || bad_color || bad_flags || info.channel != 4)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        if (layout.extent == 4 && !session.enabledFeatures().texture_compression_bc)
        {
            return cxx::unexpected(RenderError{kUnsupported});
        }
        if (premultiplied_srgb)
        {
            return cxx::unexpected(RenderError{kUnsupported});
        }
        const auto max_mips = std::bit_width(std::max(std::uint32_t(info.width), std::uint32_t(info.height)));
        if (info.mip_count > max_mips)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        std::uint64_t expected_offset = 0;
        for (std::uint32_t mip = 0; mip < info.mip_count; ++mip)
        {
            const auto width = std::max(1u, std::uint32_t(info.width) >> mip);
            const auto height = std::max(1u, std::uint32_t(info.height) >> mip);
            const auto rows = (height + layout.extent - 1) / layout.extent;
            const std::uint64_t row = std::uint64_t((width + layout.extent - 1) / layout.extent) * layout.bytes;
            const auto size = cxx::checkedMul<std::uint64_t>(row, std::uint64_t(rows));
            if (!size)
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
            const auto& input = info.mip_ranges[mip];
            const bool mismatch = input.width != width || input.height != height || input.size != *size ||
                                  input.offset != expected_offset;
            if (mismatch)
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
            auto total = cxx::checkedAdd(expected_offset, *size);
            if (!total)
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
            expected_offset = *total;
        }
        if (region)
        {
            if (region->mip >= info.mip_count)
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
            const auto& mip = info.mip_ranges[region->mip];
            const bool invalid_offset = region->x >= mip.width || region->y >= mip.height;
            if (invalid_offset)
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
            const bool invalid_range = !region->width || !region->height || region->width > mip.width - region->x ||
                                       region->height > mip.height - region->y;
            const bool invalid_blocks = region->x % layout.extent || region->y % layout.extent ||
                                        (region->width % layout.extent && region->width != mip.width - region->x) ||
                                        (region->height % layout.extent && region->height != mip.height - region->y);
            if (invalid_range || invalid_blocks)
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
            const auto rows = (region->height + layout.extent - 1) / layout.extent;
            const std::uint64_t row = ((region->width + layout.extent - 1) / layout.extent) * layout.bytes;
            const bool invalid_pitch = region->row_pitch < row || region->row_pitch % layout.bytes;
            if (invalid_pitch)
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
            auto stride = cxx::checkedMul<std::uint64_t>(rows - 1, region->row_pitch);
            if (!stride)
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
            auto total = cxx::checkedAdd(*stride, row);
            if (!total)
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
            expected_offset = *total;
        }
        if (expected_offset != source.size() || source.empty())
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        const auto properties = session.physicalDevice().formatProperties(layout.native);
        constexpr auto required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                                  VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
        if ((properties.optimalTilingFeatures & required) != required)
        {
            return cxx::unexpected(RenderError{kUnsupported});
        }
        const ImageDescription description{
            {std::uint32_t(info.width), std::uint32_t(info.height)},
            layout.native,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_SAMPLE_COUNT_1_BIT,
            info.mip_count,
            1
        };
        std::vector<ImageTransferRegion> regions;
        if (region)
        {
            regions.push_back(
                {0,
                 region->row_pitch,
                 layout.extent,
                 layout.bytes,
                 {VK_IMAGE_ASPECT_COLOR_BIT, region->mip, 0, 1},
                 {std::int32_t(region->x), std::int32_t(region->y), 0},
                 {region->width, region->height, 1}}
            );
        }
        else
        {
            for (std::uint32_t mip = 0; mip < info.mip_count; ++mip)
            {
                const auto& input = info.mip_ranges[mip];
                const auto pitch = std::uint64_t((input.width + layout.extent - 1) / layout.extent) * layout.bytes;
                regions.push_back(
                    {input.offset,
                     pitch,
                     layout.extent,
                     layout.bytes,
                     {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1},
                     {},
                     {input.width, input.height, 1}}
                );
            }
        }
        auto image = session.allocator().createImage(description);
        if (!image)
        {
            return cxx::unexpected(image.error());
        }
        return ImagePreparation{std::move(*image), std::move(regions)};
    }

    // Exclusive reservation of an existing Storage candidate. No transfer, queue or async phase is owned here.
    struct TextureStorage::Candidate final
    {
        TextureStorage& storage;
        std::unique_ptr<TextureData> version;
        TextureId texture;
        const TextureData* base = nullptr;

        ~Candidate() noexcept
        {
            if (version)
            {
                storage.rollback(*this);
            }
        }
    };

    struct TextureStorage::Use final
    {
        TextureStorage* storage;
        TextureId id;
        PreparedStorageBinding* binding;
        std::uint32_t lod;
        std::size_t position;
        std::array<float, 4> parameters{};

        void update(const TextureData* value) noexcept
        {
            parameters = value
                             ? std::array<
                                   float,
                                   4>{float(value->descriptorIndex()), float(value->info().color_space), float(lod), 0}
                             : std::array<float, 4>{0, 0, 0, 0};
            binding->parameters = std::as_bytes(std::span(parameters));
            binding->publishUpdate();
        }

        ~Use() noexcept
        {
            if (storage)
            {
                storage->unsubscribe(*this);
            }
        }
    };

    void TextureStorage::unsubscribe(Use& use) noexcept
    {
        auto& subscriptions = textures_.find(use.id.local)->uses;
        subscriptions[use.position] = subscriptions.back();
        subscriptions[use.position]->position = use.position;
        subscriptions.pop_back();
    }

    void TextureStorage::disconnect(TextureRecord& record) noexcept
    {
        for (auto* use : record.uses)
        {
            use->storage = nullptr;
            use->update(nullptr);
        }
        record.uses.clear();
    }

    RenderResult<std::pair<rdesc::TextureInfo, std::uint32_t>> TextureStorage::bind(
        TextureId id,
        PreparedStorageBinding& binding
    ) noexcept
    {
        auto* record = id.storage == id_ ? textures_.find(id.local) : nullptr;
        if (!record || !record->current)
        {
            return std::pair{rdesc::TextureInfo{}, 0u};
        }
        auto use = std::make_unique<Use>(this, id, &binding, 0, record->uses.size());
        record->uses.push_back(use.get());
        use->update(record->current.get());
        binding.reference = {use.release(), [](void* value) noexcept { delete static_cast<Use*>(value); }};
        return std::pair{record->current->info(), record->current->descriptorIndex()};
    }

    RenderResult<void> TextureStorage::prepare(
        const NativeResourcePlan& plan,
        const TextureBinding& argument,
        PreparedStorageBinding& binding
    ) noexcept
    {
        if (argument.texture.storage != id_)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        auto* record = textures_.find(argument.texture.local);
        if (!record || !record->current)
        {
            return cxx::unexpected(RenderError{kWrongOwner});
        }
        auto& backing = *record->current;
        const auto* expected = std::get_if<GraphImageDesc>(&plan.description);
        const auto& actual = backing.info();
        if (!expected)
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        const auto format = expected->format == rdesc::ETextureFormat::RGBA8_UNORM  ? VK_FORMAT_R8G8B8A8_UNORM
                            : expected->format == rdesc::ETextureFormat::RGBA8_SRGB ? VK_FORMAT_R8G8B8A8_SRGB
                                                                                    : VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
        const bool mismatch = expected->width != actual.width || expected->height != actual.height ||
                              expected->mips != actual.mip_count || expected->layers != actual.layers ||
                              format != backing.image().format() || argument.lod >= actual.mip_count;
        if (mismatch)
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        auto use = std::make_unique<Use>(this, argument.texture, &binding, argument.lod, record->uses.size());
        record->uses.push_back(use.get());
        use->update(record->current.get());
        static const ShaderBlock parameters = []
        {
            static constexpr rdesc::DataGpuField fields[]{{"control", rdesc::EDataScalar::FLOAT, 0, 4, 1, 0, 0}};
            return ShaderBlock::from({rdesc::EDataLayout::STD430, 16, 16, fields, {}, {}, 0});
        }();
        binding.parameter_layout = &parameters;
        binding.reference = {use.release(), [](void* value) noexcept { delete static_cast<Use*>(value); }};
        return {};
    }

    TextureStorage::TextureStorage(DeviceSession& session, StorageId id, TextureStorageConfig config) noexcept
        : session_(session), id_(id), residency_(std::make_shared<Residency>()), config_(config)
    {
    }

    StorageFactory textureStorageFactory() noexcept
    {
        static const lux::render::TextureStorage::Ops ops{
            [](const void* value, TextureId id) noexcept -> RenderResult<TexturePublished>
            {
                auto* record = static_cast<const vulkan::TextureStorage*>(value)->find(id);
                if (!record)
                {
                    return cxx::unexpected(RenderError{kStorageIdentity});
                }
                return TexturePublished{id, record->revision()};
            },
            [](void* value, TextureId id, std::uint64_t revision) noexcept
            { return static_cast<vulkan::TextureStorage*>(value)->release(id, revision); }
        };
        static const StorageBindingOps binding_ops{
            [](void* storage, const NativeResourcePlan& plan, const void* argument, PreparedStorageBinding& binding
            ) noexcept
            {
                return static_cast<TextureStorage*>(storage)
                    ->prepare(plan, *static_cast<const TextureBinding*>(argument), binding);
            }
        };
        static const auto compatible = +[](const vulkan::TextureStorage& value, const TextureStorageConfig& config
                                        ) noexcept { return value.compatibleConfig(config); };
        static const auto factory = makeStorageFactory<
            lux::render::TextureStorage,
            vulkan::TextureStorage,
            DeviceSession,
            &vulkan::TextureStorage::create,
            compatible>(ops, &binding_ops);
        return factory;
    }

    RenderResult<std::unique_ptr<TextureStorage>> TextureStorage::create(
        DeviceSession& session,
        StorageId id,
        const TextureStorageConfig& config
    ) noexcept
    {
        if (!session.enabledFeatures().texture_descriptors)
        {
            return cxx::unexpected(RenderError{kUnsupported});
        }
        auto storage = std::unique_ptr<TextureStorage>(new TextureStorage(session, id, config));
        if (auto result = storage->initialize(); !result)
        {
            return cxx::unexpected(result.error());
        }
        return storage;
    }

    RenderResult<void> TextureStorage::initialize() noexcept
    {
        auto table = session_.sampledImageArray();
        if (!table)
        {
            return cxx::unexpected(table.error());
        }
        descriptors_ = std::move(*table);
        return {};
    }

    TextureStorage::~TextureStorage() noexcept
    {
        for (auto& record : textures_)
        {
            disconnect(record);
            retire(record.current);
        }
    }

    VkDescriptorSetLayout TextureStorage::layout() const noexcept
    {
        return descriptors_->layout();
    }

    std::size_t TextureStorage::residentVersions() const noexcept
    {
        return residency_->versions;
    }

    std::uint64_t TextureStorage::residentBytes() const noexcept
    {
        return residency_->bytes;
    }

    std::uint64_t TextureStorage::residentPeak() const noexcept
    {
        return residency_->peak;
    }

    const TextureStorage::TextureData* TextureStorage::find(TextureId id) const noexcept
    {
        if (id.storage != id_)
        {
            return nullptr;
        }
        const auto* record = textures_.find(id.local);
        return record ? record->current.get() : nullptr;
    }

    RenderResult<const TextureStorage::TextureData*> TextureStorage::acquire(TextureId id) noexcept
    {
        if (id.storage != id_)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        auto* record = textures_.find(id.local);
        if (!record || !record->current)
        {
            return cxx::unexpected(RenderError{kWrongOwner});
        }
        return record->current.get();
    }

    RenderResult<void> TextureStorage::admit(std::uint64_t minimum_bytes, bool) noexcept
    {
        if (auto collected = session_.collect(); !collected)
        {
            return cxx::unexpected(collected.error());
        }
        const bool is_version_budget_full =
            config_.resident_versions_budget && residency_->versions >= config_.resident_versions_budget;
        const bool is_byte_budget_full =
            config_.resident_byte_budget && minimum_bytes > config_.resident_byte_budget - residency_->bytes;
        const bool full =
            is_version_budget_full || is_byte_budget_full || descriptors_->occupied() == descriptors_->capacity();
        if (full)
        {
            return cxx::unexpected(RenderError{kCapacity});
        }
        return {};
    }

    RenderResult<std::unique_ptr<TextureStorage::Candidate>> TextureStorage::reserve(
        std::unique_ptr<Image> image,
        const rdesc::TextureInfo& info,
        TextureId id,
        std::uint64_t expected
    ) noexcept
    {
        auto* record = textures_.find(id.local);
        if (id.isNull())
        {
            if (expected != 0)
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
        }
        else
        {
            if (!record || !record->current)
            {
                return cxx::unexpected(RenderError{kWrongOwner});
            }
            const bool conflict = record->updating || record->current->revision_ != expected || expected == UINT64_MAX;
            if (conflict)
            {
                return cxx::unexpected(RenderError{kBusy});
            }
            const auto& previous = record->current->info_;
            const auto& candidate = info;
            const bool incompatible_consumers =
                !record->uses.empty() &&
                (previous.width != candidate.width || previous.height != candidate.height ||
                 previous.mip_count != candidate.mip_count || previous.pixel_format != candidate.pixel_format ||
                 previous.layers != candidate.layers);
            if (incompatible_consumers)
            {
                return cxx::unexpected(RenderError{kUnsupported});
            }
        }
        if (auto collected = session_.collect(); !collected)
        {
            return cxx::unexpected(collected.error());
        }
        const auto bytes = image->memoryBinding().size;
        const bool is_byte_budget_full =
            config_.resident_byte_budget && bytes > config_.resident_byte_budget - residency_->bytes;
        if (is_byte_budget_full)
        {
            return cxx::unexpected(RenderError{kCapacity});
        }
        auto descriptor = descriptors_->allocate();
        if (!descriptor)
        {
            return cxx::unexpected(descriptor.error());
        }
        auto content =
            std::make_unique<TextureData>(residency_, std::move(image), std::move(*descriptor), info, expected + 1);
        auto& version = *content;
        version.retirement_ = session_.retirement().reserve();
        const auto device = session_.nativeDevice();
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = version.image_->native();
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = version.image_->format();
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, version.info_.mip_count, 0, 1};
        auto result = vkCreateImageView(device, &view, nullptr, &version.view_);
        if (result != VK_SUCCESS)
        {
            return cxx::unexpected(nativeError(result));
        }
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.maxLod = float(version.info_.mip_count - 1);
        result = version.sampler_.tryCreate(device, sampler);
        if (result != VK_SUCCESS)
        {
            return cxx::unexpected(nativeError(result));
        }
        // The descriptor becomes reachable only after the required upload submissions are accepted.
        version.descriptor_.write(version.view_, version.sampler_);
        if (id.isNull())
        {
            id = TextureId{id_, textures_.emplace()};
        }
        record = textures_.find(id.local);
        record->updating = true;
        return std::make_unique<Candidate>(*this, std::move(content), id, nullptr);
    }

    void TextureStorage::rollback(Candidate& candidate) noexcept
    {
        auto& record = *textures_.find(candidate.texture.local);
        record.updating = false;
        if (!record.current)
        {
            textures_.erase(candidate.texture.local);
        }
        retire(candidate.version);
    }

    TextureStorage::UploadResult TextureStorage::upload(TextureWrite input) noexcept
    {
        if (input.texture.storage != id_)
        {
            co_return cxx::unexpected(RenderError{kStorageIdentity});
        }
        const auto source = input.source.bytes().view();
        if (auto admitted = admit(source.size(), input.texture.isNull()); !admitted)
        {
            co_return cxx::unexpected(admitted.error());
        }
        auto prepared = prepareImage(input.info, source);
        if (!prepared)
        {
            co_return cxx::unexpected(prepared.error());
        }
        auto candidate = reserve(std::move(prepared->first), input.info, input.texture, input.expected_revision);
        if (!candidate)
        {
            co_return cxx::unexpected(candidate.error());
        }
        const TexturePublished result{(*candidate)->texture, (*candidate)->version->revision_};
        auto uploaded = co_await session_.uploads().writeImage(
            *(*candidate)->version->image_,
            std::move(input.source),
            std::move(prepared->second)
        );
        if (!uploaded)
        {
            co_return cxx::unexpected(uploaded.error());
        }
        const auto stop = co_await stdexec::read_env(stdexec::get_stop_token);
        if (stop.stop_requested())
        {
            co_return cxx::unexpected(RenderError{kOutcomeUnknown});
        }
        publish(**candidate);
        co_return result;
    }

    TextureStorage::UploadResult TextureStorage::upload(TextureRegionWrite input) noexcept
    {
        if (input.texture.storage != id_)
        {
            co_return cxx::unexpected(RenderError{kStorageIdentity});
        }
        auto* record = textures_.find(input.texture.local);
        if (!record || !record->current)
        {
            co_return cxx::unexpected(RenderError{kWrongOwner});
        }
        if (record->updating || record->current->revision_ != input.expected_revision)
        {
            co_return cxx::unexpected(RenderError{kBusy});
        }
        const auto& info = record->current->info_;
        const auto last = info.mip_ranges[info.mip_count - 1];
        if (auto admitted = admit(last.offset + last.size, false); !admitted)
        {
            co_return cxx::unexpected(admitted.error());
        }
        auto prepared = prepareImage(info, input.source.bytes().view(), input.region);
        if (!prepared)
        {
            co_return cxx::unexpected(prepared.error());
        }
        auto candidate = reserve(std::move(prepared->first), info, input.texture, input.expected_revision);
        if (!candidate)
        {
            co_return cxx::unexpected(candidate.error());
        }
        (*candidate)->base = record->current.get();
        const TexturePublished result{input.texture, (*candidate)->version->revision_};
        auto uploaded = co_await session_.uploads().writeImage(
            *(*candidate)->version->image_,
            std::move(input.source),
            std::move(prepared->second),
            (*candidate)->base->image_.get()
        );
        if (!uploaded)
        {
            co_return cxx::unexpected(uploaded.error());
        }
        const auto stop = co_await stdexec::read_env(stdexec::get_stop_token);
        if (stop.stop_requested())
        {
            co_return cxx::unexpected(RenderError{kOutcomeUnknown});
        }
        publish(**candidate);
        co_return result;
    }

    void TextureStorage::retire(std::unique_ptr<TextureData>& value) noexcept
    {
        if (value && !session_.retirement().retire(value->retirement_, value))
        {
            std::terminate(); // This physical candidate reserved its own retirement entry before submission.
        }
    }

    void TextureStorage::publish(Candidate& candidate) noexcept
    {
        auto& record = *textures_.find(candidate.texture.local);
        retire(record.current);
        record.current = std::move(candidate.version);
        record.updating = false;
        for (auto* use : record.uses)
        {
            use->update(record.current.get());
        }
    }

    RenderResult<void> TextureStorage::release(TextureId id, std::uint64_t expected) noexcept
    {
        if (id.storage != id_)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        auto* record = textures_.find(id.local);
        if (!record || !record->current)
        {
            return cxx::unexpected(RenderError{kWrongOwner});
        }
        if (record->updating || record->current->revision_ != expected)
        {
            return cxx::unexpected(RenderError{kBusy});
        }
        disconnect(*record);
        retire(record->current);
        textures_.erase(id.local);
        return {};
    }

} // namespace lux::render::vulkan

namespace lux::render
{
    StorageTraits<vulkan::TextureStorage>::Result StorageTraits<vulkan::TextureStorage>::upload(
        vulkan::TextureStorage& storage,
        TextureWrite input
    ) noexcept
    {
        return storage.upload(std::move(input));
    }

    StorageTraits<vulkan::TextureStorage>::Result StorageTraits<vulkan::TextureStorage>::upload(
        vulkan::TextureStorage& storage,
        TextureRegionWrite input
    ) noexcept
    {
        return storage.upload(std::move(input));
    }
} // namespace lux::render
