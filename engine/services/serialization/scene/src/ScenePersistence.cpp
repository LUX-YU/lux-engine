#include <lux/engine/scene/ScenePersistence.hpp>
#include <lux/engine/scene/SceneContent.hpp>
#include <lux/engine/assets/AssetRepository.hpp>
#include <lux/engine/serialization/Serialization.hpp>

#include <algorithm>
#include <array>
#include <unordered_set>

namespace lux::scene
{
    namespace
    {
        constexpr auto kInvalid = error::errorId("lux.scene.content.invalid");
        constexpr auto kCapacity = error::errorId("lux.scene.content.capacity");
        constexpr std::string_view kContentMagic = "LXSCONT1";

        cxx::expected<void, error::Error>
        validateBlockUpdates(const SceneData& base, std::vector<SceneBlockUpdate>& updates) noexcept
        {
            std::ranges::sort(updates, {}, &SceneBlockUpdate::block);
            auto next = base.blocks.size();
            std::uint32_t previous = UINT32_MAX;
            for (const auto& update : updates)
            {
                const bool invalid_block = update.block == UINT32_MAX ||
                    update.block == previous || update.block > next;
                const bool reuses_deleted_block = update.block < base.blocks.size() &&
                    base.blocks[update.block].size == 0 && !update.objects.empty();
                if (invalid_block || reuses_deleted_block || update.objects.size() != update.summaries.size())
                    return cxx::unexpected(error::Error{kInvalid, {update.block}});
                if (update.block == next) ++next;
                previous = update.block;
                for (std::size_t object = 0; object < update.objects.size(); ++object)
                {
                    if (update.objects[object].id != update.summaries[object].id)
                        return cxx::unexpected(error::Error{kInvalid, {update.block}});
                }
            }
            return {};
        }

        template <class Blocks>
        cxx::expected<void, error::Error>
        validateSceneObjects(
            const SceneDescription& description, const SceneStreamingData& streaming, const Blocks& summaries
        ) noexcept
        {
            std::unordered_set<world::WorldObjectId, world::WorldObjectIdHash> objects;
            if (streaming.block_groups.size() != summaries.size())
                return cxx::unexpected(error::Error{kInvalid});
            for (std::uint32_t block = 0; block < summaries.size(); ++block)
            {
                for (const auto& object : summaries[block])
                {
                    if (!world::isValid(object.id) || !objects.insert(object.id).second)
                        return cxx::unexpected(error::Error{kInvalid, {block}});
                    for (const auto& bounds : object.geometry)
                    {
                        const auto group = streaming.block_groups[block];
                        const bool invalid_owner = group >= streaming.groups.size() ||
                            streaming.groups[group].geometry != bounds.geometry;
                        if (bounds.geometry >= description.world.supported_geometries.size() || invalid_owner)
                            return cxx::unexpected(error::Error{kInvalid, {bounds.geometry}});
                    }
                }
            }
            return {};
        }

        exec::task<cxx::expected<void, error::Error>>
        validateContentHeader(
            engine::assets::AssetRepository& assets, const engine::assets::AssetDataRef<SceneData>& base
        ) noexcept
        {
            auto header = co_await assets.readRange(base, 0, 28);
            if (!header) co_return cxx::unexpected(header.error());
            serialization::BinaryReader reader(header->view());
            std::array<std::byte, 8> magic{};
            std::array<std::byte, 16> identity{};
            if (!reader.readBytes(magic) || !std::ranges::equal(magic, std::as_bytes(std::span(kContentMagic))))
                co_return cxx::unexpected(error::Error{kInvalid});
            auto version = reader.readUnsigned<std::uint32_t>();
            if (!version || *version != 1 || !reader.readBytes(identity) ||
                !std::ranges::equal(identity, base.assetId().uuid().as_bytes()))
                co_return cxx::unexpected(error::Error{kInvalid});
            co_return cxx::expected<void, error::Error>{};
        }

        exec::task<cxx::expected<void, error::Error>>
        mergeSceneSummaries(
            engine::assets::AssetRepository& assets, const engine::assets::AssetDataRef<SceneData>& base,
            const SceneDescription& description, const std::vector<SceneBlockUpdate>& updates,
            const SceneStreamingData& streaming
        ) noexcept
        {
            const auto limit = assets.decodeLimits().max_decoded_bytes;
            const auto count = updates.empty() ? base->blocks.size() :
                std::max(base->blocks.size(), std::size_t(updates.back().block) + 1);
            constexpr auto entry_bytes =
                sizeof(std::vector<SceneObjectSummary>) + sizeof(std::span<const SceneObjectSummary>);
            if (count > limit / entry_bytes)
                co_return cxx::unexpected(error::Error{kCapacity});
            std::size_t used = count * entry_bytes;
            std::vector<std::vector<SceneObjectSummary>> old_summaries(count);
            std::vector<std::span<const SceneObjectSummary>> summaries(count);
            auto& execution = execution::ExecutionRuntime::instance();
            std::size_t changed{};
            const auto& previous = base->description.world.supported_geometries;
            const auto& current = description.world.supported_geometries;
            bool remapped = current.size() < previous.size();
            for (std::size_t i = 0; i < std::min(previous.size(), current.size()); ++i)
                remapped = remapped || previous[i].name != current[i].name;
            for (std::size_t block = 0; block < count; ++block)
            {
                if (changed < updates.size() && updates[changed].block == block)
                {
                    summaries[block] = updates[changed++].summaries;
                }
                else if (block < base->blocks.size() && base->blocks[block].size)
                {
                    if (remapped) co_return cxx::unexpected(error::Error{kInvalid, {block}});
                    const auto& value = base->blocks[block];
                    if (value.summary_size > limit - used) co_return cxx::unexpected(error::Error{kCapacity});
                    auto bytes = co_await assets.readRange(base, value.offset, value.summary_size);
                    if (!bytes) co_return cxx::unexpected(bytes.error());
                    auto decoded = co_await stdexec::continues_on(stdexec::then(
                        stdexec::schedule(execution.cpuScheduler()), [&]() noexcept
                        {
                            if (cxx::algorithm::Sha256::hash(bytes->view()) != value.summary_digest)
                                return cxx::expected<std::vector<SceneObjectSummary>, error::Error>{
                                    cxx::unexpected(error::Error{kInvalid})};
                            return decodeSceneSummary(bytes->view(), limit - used);
                        }), execution.mainScheduler());
                    if (!decoded) co_return cxx::unexpected(decoded.error());
                    old_summaries[block] = std::move(*decoded);
                    summaries[block] = old_summaries[block];
                }
                const auto charge = [&](std::size_t size) noexcept
                {
                    if (size > limit - used) return false;
                    used += size;
                    return true;
                };
                if (!charge(summaries[block].size() * sizeof(SceneObjectSummary)))
                    co_return cxx::unexpected(error::Error{kCapacity});
                for (const auto& object : summaries[block])
                {
                    if (!charge(object.geometry.size() * sizeof(SceneGeometryBounds)) || !charge(80))
                        co_return cxx::unexpected(error::Error{kCapacity});
                }
            }
            co_return co_await stdexec::then(stdexec::schedule(execution.cpuScheduler()), [&]() noexcept
            {
                return validateSceneObjects(description, streaming, summaries);
            });
        }

        cxx::expected<std::vector<std::byte>, error::Error>
        encodeSceneRoot(
            asset::AssetInfo info, const SceneData& data, std::span<const asset::AssetAuxiliaryPayload> auxiliary,
            std::size_t limit
        ) noexcept
        {
            info.type = SceneAsset::asset_type;
            auto encoded = asset::TAssetSerDeser<SceneAsset>::encode(
                info, data, auxiliary, asset::AssetEncodeLimits{limit}
            );
            if (!encoded)
            {
                const auto id = encoded.error().code == asset::EAssetEncodeError::LIMIT_EXCEEDED ? kCapacity : kInvalid;
                return cxx::unexpected(error::Error{id});
            }
            return std::move(*encoded);
        }

        exec::task<cxx::expected<void, error::Error>>
        publishSceneRoot(
            engine::assets::AssetRepository& assets, asset::AssetContentWriter& writer, asset::AssetInfo info,
            SceneData data, std::span<const asset::AssetAuxiliaryPayload> auxiliary
        ) noexcept
        {
            auto& execution = execution::ExecutionRuntime::instance();
            data.content_file = writer.contentName();
            auto encoded = co_await stdexec::continues_on(stdexec::then(
                stdexec::schedule(execution.cpuScheduler()), [&]() noexcept
                {
                    return encodeSceneRoot(info, std::move(data), auxiliary, assets.decodeLimits().max_image_bytes);
                }), execution.mainScheduler());
            if (!encoded) co_return cxx::unexpected(encoded.error());
            auto blocking = execution.blockingIoScheduler();
            if (!blocking) co_return cxx::unexpected(execution::toError(blocking.error()));
            co_return co_await stdexec::then(stdexec::schedule(*blocking), [&]() noexcept
                -> cxx::expected<void, error::Error>
            {
                auto published = writer.publish(*encoded);
                if (!published) return cxx::unexpected(error::Error{engine::assets::kAssetWriteFailure,
                    {static_cast<std::uint64_t>(published.error())}});
                return {};
            });
        }

        exec::task<cxx::expected<void, error::Error>>
        appendSceneBlocks(
            engine::assets::AssetRepository& assets, asset::AssetContentWriter& writer,
            SceneData& data, const std::vector<SceneBlockUpdate>& updates, const asset::AssetId* new_identity
        ) noexcept
        {
            auto& execution = execution::ExecutionRuntime::instance();
            auto blocking = execution.blockingIoScheduler();
            if (!blocking) co_return cxx::unexpected(execution::toError(blocking.error()));
            if (new_identity)
            {
                std::array<std::byte, 28> header{};
                std::copy(kContentMagic.begin(), kContentMagic.end(), reinterpret_cast<char*>(header.data()));
                header[8] = std::byte{1};
                std::ranges::copy(new_identity->uuid().as_bytes(), header.begin() + 12);
                auto written = co_await stdexec::then(stdexec::schedule(*blocking), [&]() noexcept
                {
                    return writer.append(header);
                });
                if (!written || *written != 0) co_return cxx::unexpected(error::Error{kInvalid});
            }
            if (!updates.empty())
            {
                data.blocks.resize(std::max(data.blocks.size(), std::size_t(updates.back().block) + 1));
            }
            for (const auto& update : updates)
            {
                if (update.objects.empty()) { data.blocks[update.block] = {}; continue; }
                std::vector<std::byte> bytes;
                auto& recorded = data.blocks[update.block];
                auto encoded = co_await stdexec::continues_on(stdexec::then(
                    stdexec::schedule(execution.cpuScheduler()), [&]() noexcept
                    {
                        return encodeSceneBlock(update.objects, update.summaries, bytes, recorded,
                            assets.decodeLimits().max_image_bytes);
                    }), execution.mainScheduler());
                if (!encoded) co_return encoded;
                auto offset = co_await stdexec::then(stdexec::schedule(*blocking), [&]() noexcept
                {
                    return writer.append(bytes);
                });
                if (!offset) co_return cxx::unexpected(error::Error{engine::assets::kAssetWriteFailure,
                    {static_cast<std::uint64_t>(offset.error())}});
                recorded.offset = *offset;
            }
            std::erase_if(data.bootstrap_blocks, [&](auto block) noexcept
            {
                return block >= data.blocks.size() || !data.blocks[block].size;
            });
            co_return cxx::expected<void, error::Error>{};
        }

        exec::task<cxx::expected<void, error::Error>>
        copyReferencedBlocks(
            engine::assets::AssetRepository& assets, const engine::assets::AssetDataRef<SceneData>& base,
            asset::AssetContentWriter& writer, SceneData& data, asset::AssetId identity, std::size_t buffer_bytes
        ) noexcept
        {
            if (!buffer_bytes || buffer_bytes > assets.decodeLimits().max_image_bytes)
                co_return cxx::unexpected(error::Error{kCapacity});
            auto& execution = execution::ExecutionRuntime::instance();
            auto blocking = execution.blockingIoScheduler();
            if (!blocking) co_return cxx::unexpected(execution::toError(blocking.error()));
            auto started = co_await appendSceneBlocks(assets, writer, data, {}, &identity);
            if (!started) co_return started;
            for (std::size_t i = 0; i < base->blocks.size(); ++i)
            {
                const auto& previous = base->blocks[i];
                if (!previous.size) continue;
                cxx::algorithm::Sha256 full, summary;
                for (std::uint64_t position = 0; position < previous.size;)
                {
                    const auto size = std::min<std::uint64_t>(buffer_bytes, previous.size - position);
                    auto bytes = co_await assets.readRange(base, previous.offset + position, size);
                    if (!bytes) co_return cxx::unexpected(bytes.error());
                    co_await stdexec::then(stdexec::schedule(execution.cpuScheduler()), [&]() noexcept
                    {
                        full.update(bytes->view());
                        if (position < previous.summary_size)
                            summary.update(bytes->view().first(static_cast<std::size_t>(
                                std::min(size, previous.summary_size - position))));
                    });
                    auto offset = co_await stdexec::then(stdexec::schedule(*blocking), [&]() noexcept
                    {
                        return writer.append(bytes->view());
                    });
                    if (!offset) co_return cxx::unexpected(error::Error{engine::assets::kAssetWriteFailure,
                        {static_cast<std::uint64_t>(offset.error())}});
                    if (position == 0) data.blocks[i].offset = *offset;
                    position += size;
                }
                if (full.digest() != previous.digest || summary.digest() != previous.summary_digest)
                    co_return cxx::unexpected(error::Error{kInvalid, {i}});
            }
            co_return cxx::expected<void, error::Error>{};
        }
    }

    exec::task<cxx::expected<void, error::Error>>
    saveScene(
        engine::assets::AssetRepository& assets, engine::assets::AssetRef<SceneAsset> asset,
        engine::assets::AssetDataRef<SceneData> base, SceneDescription description,
        std::vector<SceneBlockUpdate> updates, SceneStreamingData streaming
    ) noexcept
    {
        const bool invalid_source = !asset || !base || asset.id() != base.assetId() || !assets.isCurrent(base);
        if (invalid_source) co_return cxx::unexpected(error::Error{kInvalid});
        auto valid = validateBlockUpdates(*base, updates);
        if (!valid) co_return valid;
        valid = co_await validateContentHeader(assets, base);
        if (!valid) co_return valid;
        valid = co_await mergeSceneSummaries(assets, base, description, updates, streaming);
        if (!valid) co_return valid;
        SceneData data = *base;
        data.description = std::move(description);
        data.streaming = std::move(streaming);
        auto original = base.originalAsset();
        co_return co_await assets.withContentWriter(asset, base, asset::EAssetContentWrite::APPEND,
            [&](asset::AssetContentWriter& writer) noexcept -> exec::task<cxx::expected<void, error::Error>>
            {
                auto appended = co_await appendSceneBlocks(assets, writer, data, updates, nullptr);
                if (!appended) co_return appended;
                co_return co_await publishSceneRoot(assets, writer, original->info(), std::move(data),
                    original->auxiliaryPayloads());
            });
    }

    exec::task<cxx::expected<engine::assets::AssetRef<SceneAsset>, error::Error>>
    createSceneAsset(
        engine::assets::AssetRepository& assets, std::string path, asset::AssetInfo info,
        SceneDescription description, std::vector<SceneBlockUpdate> updates, SceneStreamingData streaming
    ) noexcept
    {
        SceneData data;
        data.description = std::move(description);
        data.streaming = std::move(streaming);
        auto valid = validateBlockUpdates(data, updates);
        if (!valid) co_return cxx::unexpected(valid.error());
        std::vector<std::span<const SceneObjectSummary>> summaries;
        summaries.reserve(updates.size());
        for (const auto& update : updates) summaries.push_back(update.summaries);
        valid = validateSceneObjects(data.description, data.streaming, summaries);
        if (!valid) co_return cxx::unexpected(valid.error());
        co_return co_await assets.createContent<SceneAsset>(std::move(path), info,
            [&](asset::AssetContentWriter& writer) noexcept -> exec::task<cxx::expected<void, error::Error>>
            {
                auto appended = co_await appendSceneBlocks(assets, writer, data, updates, &info.id);
                if (!appended) co_return appended;
                co_return co_await publishSceneRoot(assets, writer, info, std::move(data), {});
            });
    }

    exec::task<cxx::expected<engine::assets::AssetRef<SceneAsset>, error::Error>>
    copySceneAsset(
        engine::assets::AssetRepository& assets, engine::assets::AssetDataRef<SceneData> base,
        std::string path, asset::AssetInfo info
    ) noexcept
    {
        if (!base || base.assetId() == info.id) co_return cxx::unexpected(error::Error{kInvalid});
        auto valid = co_await validateContentHeader(assets, base);
        if (!valid) co_return cxx::unexpected(valid.error());
        SceneData data = *base;
        auto original = base.originalAsset();
        co_return co_await assets.createContent<SceneAsset>(std::move(path), info,
            [&](asset::AssetContentWriter& writer) noexcept -> exec::task<cxx::expected<void, error::Error>>
            {
                auto copied = co_await copyReferencedBlocks(assets, base, writer, data, info.id,
                    assets.decodeLimits().max_image_bytes);
                if (!copied) co_return copied;
                co_return co_await publishSceneRoot(
                    assets, writer, info, std::move(data), original->auxiliaryPayloads()
                );
            });
    }

    exec::task<cxx::expected<void, error::Error>>
    compactSceneAsset(
        engine::assets::AssetRepository& assets, engine::assets::AssetRef<SceneAsset> asset,
        engine::assets::AssetDataRef<SceneData> base, std::size_t copy_buffer_bytes
    ) noexcept
    {
        const bool invalid_source = !asset || !base || asset.id() != base.assetId() || !assets.isCurrent(base);
        if (invalid_source) co_return cxx::unexpected(error::Error{kInvalid});
        auto valid = co_await validateContentHeader(assets, base);
        if (!valid) co_return valid;
        SceneData data = *base;
        auto original = base.originalAsset();
        co_return co_await assets.withContentWriter(asset, base, asset::EAssetContentWrite::COMPACT,
            [&](asset::AssetContentWriter& writer) noexcept -> exec::task<cxx::expected<void, error::Error>>
            {
                auto copied = co_await copyReferencedBlocks(assets, base, writer, data, asset.id(), copy_buffer_bytes);
                if (!copied) co_return copied;
                co_return co_await publishSceneRoot(assets, writer, original->info(), std::move(data),
                    original->auxiliaryPayloads());
            });
    }
} // namespace lux::scene
