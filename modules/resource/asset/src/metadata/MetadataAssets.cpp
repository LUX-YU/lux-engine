#include <lux/engine/resource/asset/animation/AnimationClipAsset.hpp>
#include <lux/engine/resource/asset/animation/SkeletonAsset.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>

#include <lux/engine/resource/asset/CookedAssetImage.hpp>
#include <lux/engine/resource/asset/animation/AnimationClipDescriptionCodec.hpp>
#include <lux/engine/resource/asset/animation/SkeletonDescriptionCodec.hpp>
#include <lux/engine/resource/asset/detail/CookedAssetWriter.hpp>
#include <lux/engine/resource/asset/mesh/MeshDescriptionCodec.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <string>
#include <utility>

namespace lux::asset
{
    namespace
    {
        [[nodiscard]] AssetDecodeFailure decodeFailure(
            EAssetDecodeError code,
            std::size_t offset = 0U
        ) noexcept
        {
            return AssetDecodeFailure{code, offset};
        }

        [[nodiscard]] AssetEncodeFailure encodeFailure(
            EAssetEncodeError code,
            std::size_t offset = 0U
        ) noexcept
        {
            return AssetEncodeFailure{code, offset};
        }

        [[nodiscard]] bool canonicalizeAuxiliary(std::vector<AssetAuxiliaryPayload>& auxiliary) noexcept
        {
            std::sort(
                auxiliary.begin(),
                auxiliary.end(),
                [](const auto& left, const auto& right) noexcept { return left.tag < right.tag; }
            );
            for (std::size_t index = 0U; index < auxiliary.size(); ++index)
            {
                const bool invalid = auxiliary[index].tag == 0U || auxiliary[index].bytes.empty();
                const bool duplicate = index != 0U && auxiliary[index - 1U].tag == auxiliary[index].tag;
                if (invalid || duplicate)
                    return false;
            }
            return true;
        }

        [[nodiscard]] bool validMesh(const lux::rdesc::Mesh& mesh) noexcept
        {
            if (mesh.vertices.empty() || mesh.indices.empty() || mesh.indices.size() % 3U != 0U ||
                mesh.vertices.size() > detail::kMaxMeshVertexCount ||
                mesh.indices.size() > detail::kMaxMeshIndexCount ||
                mesh.lods.size() > detail::kMaxMeshLodCount)
            {
                return false;
            }
            for (const auto index : mesh.indices)
                if (index >= mesh.vertices.size()) return false;
            for (const auto& lod : mesh.lods)
            {
                if (!std::isfinite(lod.error) || lod.indices.size() > detail::kMaxMeshIndexCount)
                    return false;
                for (const auto index : lod.indices)
                    if (index >= mesh.vertices.size()) return false;
            }
            return !mesh.bounds || (mesh.bounds->min.allFinite() && mesh.bounds->max.allFinite());
        }

        [[nodiscard]] bool validSkeleton(const lux::rdesc::Skeleton& skeleton) noexcept
        {
            if (skeleton.bones.empty() || skeleton.bones.size() > detail::kMaxSkelBoneCount ||
                !skeleton.global_transform.matrix().allFinite())
            {
                return false;
            }
            for (std::size_t index = 0U; index < skeleton.bones.size(); ++index)
            {
                const auto& bone = skeleton.bones[index];
                if (bone.name.empty() || bone.name.size() > detail::kMaxSkelStringLen ||
                    bone.parent_index >= static_cast<std::int32_t>(index) ||
                    !bone.bind_local.matrix().allFinite() || !bone.inv_bind_world.matrix().allFinite())
                {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool validAnimation(const lux::rdesc::AnimationClip& clip) noexcept
        {
            if (clip.name.empty() || clip.name.size() > detail::kMaxAcStringLen ||
                !std::isfinite(clip.duration) || clip.duration <= 0.0F ||
                clip.tracks.size() > detail::kMaxAcTrackCount)
            {
                return false;
            }
            for (const auto& track : clip.tracks)
            {
                const bool count_mismatch = track.times_t.size() != track.translations.size() ||
                    track.times_r.size() != track.rotations.size() ||
                    track.times_s.size() != track.scales.size();
                const bool count_overflow = track.times_t.size() > detail::kMaxAcKeyCount ||
                    track.times_r.size() > detail::kMaxAcKeyCount ||
                    track.times_s.size() > detail::kMaxAcKeyCount;
                if (track.bone_index < 0 || count_mismatch || count_overflow)
                    return false;
                const auto sortedFinite = [](const std::vector<float>& times) noexcept {
                    float previous = -std::numeric_limits<float>::infinity();
                    for (const float time : times)
                    {
                        if (!std::isfinite(time) || time < previous)
                            return false;
                        previous = time;
                    }
                    return true;
                };
                if (!sortedFinite(track.times_t) || !sortedFinite(track.times_r) || !sortedFinite(track.times_s))
                    return false;
                for (const auto& value : track.translations) if (!value.allFinite()) return false;
                for (const auto& value : track.rotations) if (!value.coeffs().allFinite()) return false;
                for (const auto& value : track.scales) if (!value.allFinite()) return false;
            }
            return true;
        }

        [[nodiscard]] std::size_t skeletonRetained(const lux::rdesc::Skeleton& skeleton) noexcept
        {
            std::size_t result = sizeof(skeleton) + skeleton.bones.capacity() * sizeof(lux::rdesc::Bone_t);
            for (const auto& bone : skeleton.bones)
            {
                if (bone.name.capacity() > (std::numeric_limits<std::size_t>::max)() - result)
                    return (std::numeric_limits<std::size_t>::max)();
                result += bone.name.capacity();
            }
            return result;
        }

        [[nodiscard]] std::size_t animationRetained(const lux::rdesc::AnimationClip& clip) noexcept
        {
            std::size_t result = sizeof(clip) + clip.name.capacity() +
                clip.tracks.capacity() * sizeof(lux::rdesc::BoneTrack);
            const auto add = [&result](std::size_t count, std::size_t stride) noexcept {
                if (stride != 0U && count > ((std::numeric_limits<std::size_t>::max)() - result) / stride)
                    return false;
                result += count * stride;
                return true;
            };
            for (const auto& track : clip.tracks)
            {
                if (!add(track.times_t.capacity(), sizeof(float)) ||
                    !add(track.translations.capacity(), sizeof(Eigen::Vector3f)) ||
                    !add(track.times_r.capacity(), sizeof(float)) ||
                    !add(track.rotations.capacity(), sizeof(Eigen::Quaternionf)) ||
                    !add(track.times_s.capacity(), sizeof(float)) ||
                    !add(track.scales.capacity(), sizeof(Eigen::Vector3f)))
                {
                    return (std::numeric_limits<std::size_t>::max)();
                }
            }
            return result;
        }

        template <class ConcreteAsset, class Data, class Decode, class Retained>
        [[nodiscard]] lux::cxx::expected<std::shared_ptr<const ConcreteAsset>, AssetDecodeFailure>
        decodeMetadata(
            AssetId requested,
            lux::cxx::SharedBytes<> bytes,
            const AssetDecodeLimits& limits,
            Decode&& decode,
            Retained&& retained
        ) noexcept
        {
            auto image = inspectCookedAssetImage(requested, std::move(bytes), limits);
            if (!image)
                return lux::cxx::unexpected(image.error());
            if (image->magic() != ConcreteAsset::primary_magic)
                return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_MAGIC));
            if (image->metadata().legacy_type_tag != ConcreteAsset::legacy_type_tag)
                return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_TYPE));
            if (!image->data().empty() || image->information().empty())
                return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_LAYOUT));
            try
            {
                auto data = std::make_shared<Data>();
                std::string error;
                if (!decode(image->information().view(), *data, &error) || retained(*data) > limits.max_decoded_bytes)
                    return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
                std::vector<AssetAuxiliaryPayload> auxiliary(
                    image->auxiliaryPayloads().begin(),
                    image->auxiliaryPayloads().end()
                );
                return ConcreteAsset::create(
                    AssetInfo{
                        image->metadata().id,
                        ConcreteAsset::asset_type,
                        image->metadata().date,
                        image->metadata().display_name,
                        image->metadata().source_path,
                        image->metadata().source_mtime
                    },
                    std::move(data),
                    std::move(auxiliary)
                );
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::ALLOCATION_FAILURE));
            }
            catch (...)
            {
                return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
            }
        }

        template <class ConcreteAsset, class Encode>
        [[nodiscard]] lux::cxx::expected<std::vector<std::byte>, AssetEncodeFailure> encodeMetadata(
            const ConcreteAsset& asset,
            const AssetEncodeLimits& limits,
            Encode&& encode
        ) noexcept
        {
            try
            {
                const auto information = encode(asset.data());
                if (information.empty())
                    return lux::cxx::unexpected(encodeFailure(EAssetEncodeError::INVALID_PAYLOAD));
                return detail::encodeCookedAssetImage(
                    detail::CookedAssetWriteRequest{
                        ConcreteAsset::primary_magic,
                        ConcreteAsset::legacy_type_tag,
                        asset.info(),
                        information,
                        {},
                        asset.auxiliaryPayloads()
                    },
                    limits
                );
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(encodeFailure(EAssetEncodeError::ALLOCATION_FAILURE));
            }
            catch (...)
            {
                return lux::cxx::unexpected(encodeFailure(EAssetEncodeError::INVALID_PAYLOAD));
            }
        }
    } // namespace

    MeshAsset::MeshAsset(
        AssetInfo info,
        std::shared_ptr<const lux::rdesc::Mesh> data,
        std::vector<AssetAuxiliaryPayload> auxiliary
    ) noexcept
        : TAsset(std::move(info), std::move(data), std::move(auxiliary))
    {
    }

    lux::cxx::expected<std::shared_ptr<const MeshAsset>, AssetDecodeFailure> MeshAsset::create(
        AssetInfo info,
        std::shared_ptr<const lux::rdesc::Mesh> data,
        std::vector<AssetAuxiliaryPayload> auxiliary
    ) noexcept
    {
        if (info.id.isNull() || !data || !validMesh(*data) || !canonicalizeAuxiliary(auxiliary))
            return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
        info.type = asset_type;
        try
        {
            return std::shared_ptr<const MeshAsset>(
                new MeshAsset(std::move(info), std::move(data), std::move(auxiliary))
            );
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::ALLOCATION_FAILURE));
        }
    }

    SkeletonAsset::SkeletonAsset(
        AssetInfo info,
        std::shared_ptr<const lux::rdesc::Skeleton> data,
        std::vector<AssetAuxiliaryPayload> auxiliary
    ) noexcept
        : TAsset(std::move(info), std::move(data), std::move(auxiliary))
    {
    }

    lux::cxx::expected<std::shared_ptr<const SkeletonAsset>, AssetDecodeFailure> SkeletonAsset::create(
        AssetInfo info,
        std::shared_ptr<const lux::rdesc::Skeleton> data,
        std::vector<AssetAuxiliaryPayload> auxiliary
    ) noexcept
    {
        if (info.id.isNull() || !data || !validSkeleton(*data) || !canonicalizeAuxiliary(auxiliary))
            return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
        info.type = asset_type;
        try
        {
            return std::shared_ptr<const SkeletonAsset>(
                new SkeletonAsset(std::move(info), std::move(data), std::move(auxiliary))
            );
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::ALLOCATION_FAILURE));
        }
    }

    AnimationClipAsset::AnimationClipAsset(
        AssetInfo info,
        std::shared_ptr<const lux::rdesc::AnimationClip> data,
        std::vector<AssetAuxiliaryPayload> auxiliary
    ) noexcept
        : TAsset(std::move(info), std::move(data), std::move(auxiliary))
    {
    }

    lux::cxx::expected<std::shared_ptr<const AnimationClipAsset>, AssetDecodeFailure>
    AnimationClipAsset::create(
        AssetInfo info,
        std::shared_ptr<const lux::rdesc::AnimationClip> data,
        std::vector<AssetAuxiliaryPayload> auxiliary
    ) noexcept
    {
        if (info.id.isNull() || !data || !validAnimation(*data) || !canonicalizeAuxiliary(auxiliary))
            return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
        info.type = asset_type;
        try
        {
            return std::shared_ptr<const AnimationClipAsset>(
                new AnimationClipAsset(std::move(info), std::move(data), std::move(auxiliary))
            );
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::ALLOCATION_FAILURE));
        }
    }

    lux::cxx::expected<std::shared_ptr<const MeshAsset>, AssetDecodeFailure>
    TAssetSerDeser<MeshAsset>::decode(
        AssetId requested,
        lux::cxx::SharedBytes<> image,
        const AssetDecodeLimits& limits
    ) noexcept
    {
        return decodeMetadata<MeshAsset, lux::rdesc::Mesh>(
            requested,
            std::move(image),
            limits,
            &detail::decodeMeshDescription,
            [](const lux::rdesc::Mesh& mesh) noexcept {
                return lux::rdesc::meshRetainedBytes(mesh).value_or(
                    (std::numeric_limits<std::size_t>::max)()
                );
            }
        );
    }

    lux::cxx::expected<std::vector<std::byte>, AssetEncodeFailure>
    TAssetSerDeser<MeshAsset>::encode(const MeshAsset& asset, const AssetEncodeLimits& limits) noexcept
    {
        if (!validMesh(asset.data()))
            return lux::cxx::unexpected(encodeFailure(EAssetEncodeError::INVALID_ASSET));
        return encodeMetadata(asset, limits, &detail::encodeMeshDescription);
    }

    lux::cxx::expected<std::shared_ptr<const SkeletonAsset>, AssetDecodeFailure>
    TAssetSerDeser<SkeletonAsset>::decode(
        AssetId requested,
        lux::cxx::SharedBytes<> image,
        const AssetDecodeLimits& limits
    ) noexcept
    {
        return decodeMetadata<SkeletonAsset, lux::rdesc::Skeleton>(
            requested,
            std::move(image),
            limits,
            &detail::decodeSkeletonDescription,
            &skeletonRetained
        );
    }

    lux::cxx::expected<std::vector<std::byte>, AssetEncodeFailure>
    TAssetSerDeser<SkeletonAsset>::encode(
        const SkeletonAsset& asset,
        const AssetEncodeLimits& limits
    ) noexcept
    {
        if (!validSkeleton(asset.data()))
            return lux::cxx::unexpected(encodeFailure(EAssetEncodeError::INVALID_ASSET));
        return encodeMetadata(asset, limits, &detail::encodeSkeletonDescription);
    }

    lux::cxx::expected<std::shared_ptr<const AnimationClipAsset>, AssetDecodeFailure>
    TAssetSerDeser<AnimationClipAsset>::decode(
        AssetId requested,
        lux::cxx::SharedBytes<> image,
        const AssetDecodeLimits& limits
    ) noexcept
    {
        return decodeMetadata<AnimationClipAsset, lux::rdesc::AnimationClip>(
            requested,
            std::move(image),
            limits,
            &detail::decodeAnimationClipDescription,
            &animationRetained
        );
    }

    lux::cxx::expected<std::vector<std::byte>, AssetEncodeFailure>
    TAssetSerDeser<AnimationClipAsset>::encode(
        const AnimationClipAsset& asset,
        const AssetEncodeLimits& limits
    ) noexcept
    {
        if (!validAnimation(asset.data()))
            return lux::cxx::unexpected(encodeFailure(EAssetEncodeError::INVALID_ASSET));
        return encodeMetadata(asset, limits, &detail::encodeAnimationClipDescription);
    }
} // namespace lux::asset
