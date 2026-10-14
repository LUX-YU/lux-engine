#pragma once

#include <lux/engine/render/runtime/RenderClient.hpp>

namespace lux::render
{
    struct EmptyRequest final
    {
        std::uint32_t reserved = 0;
    };

    struct SceneRequest final
    {
        SceneId scene{};
    };

    struct CreateViewRequest final
    {
        SceneId scene{};
        std::array<float, 4> color{};
    };

    struct ViewRequest final
    {
        SceneId scene{};
        ViewId view{};
    };

    struct CreateTargetRequest final
    {
        TargetDesc description{};
    };

    struct ResizeTargetRequest final
    {
        TargetId target{};
        TargetDesc description{};
    };

    struct EffectRequest final
    {
        EffectId effect{};
    };

    struct AttachmentRequest final
    {
        SceneId scene{};
        ViewId view{};
        TargetId target{};
        EffectId effect{};
    };

    struct SceneCreated final
    {
        SceneId scene{};
    };

    struct ViewCreated final
    {
        ViewId view{};
    };

    struct TargetCreated final
    {
        TargetId target{};
        std::uint64_t epoch = 0;
    };

    struct EffectCreated final
    {
        EffectId effect{};
    };

    struct ReadbackResult final
    {
        ExternalDataRef pixels{};
    };

    template <> struct DataSchema<EmptyRequest>
    {
        [[nodiscard]] static const DataSchemaDescriptor& describe() noexcept;
        [[nodiscard]] static RenderResult<std::size_t> wireSize(const EmptyRequest&) noexcept { return 4; }
        [[nodiscard]] static RenderResult<void> encode(const EmptyRequest& value, cxx::BinarySpanWriter& out) noexcept;
        [[nodiscard]] static RenderResult<EmptyRequest> decode(cxx::BinaryReader& in, BorrowedWireView) noexcept;
        [[nodiscard]] static RenderResult<EmptyRequest> decode(cxx::BinaryReader& in, WireView) noexcept;
    };

    template <> struct DataSchema<SceneRequest>
    {
        [[nodiscard]] static const DataSchemaDescriptor& describe() noexcept;
        [[nodiscard]] static RenderResult<std::size_t> wireSize(const SceneRequest&) noexcept { return 8; }
        [[nodiscard]] static RenderResult<void> encode(const SceneRequest& value, cxx::BinarySpanWriter& out) noexcept;
        [[nodiscard]] static RenderResult<SceneRequest> decode(cxx::BinaryReader& in, BorrowedWireView) noexcept;
        [[nodiscard]] static RenderResult<SceneRequest> decode(cxx::BinaryReader& in, WireView) noexcept;
    };

    template <> struct DataSchema<CreateViewRequest>
    {
        [[nodiscard]] static const DataSchemaDescriptor& describe() noexcept;
        [[nodiscard]] static RenderResult<std::size_t> wireSize(const CreateViewRequest&) noexcept { return 24; }
        [[nodiscard]] static RenderResult<void>
        encode(const CreateViewRequest& value, cxx::BinarySpanWriter& out) noexcept;
        [[nodiscard]] static RenderResult<CreateViewRequest> decode(cxx::BinaryReader& in, BorrowedWireView) noexcept;
        [[nodiscard]] static RenderResult<CreateViewRequest> decode(cxx::BinaryReader& in, WireView) noexcept;
    };

    template <> struct DataSchema<ViewRequest>
    {
        [[nodiscard]] static const DataSchemaDescriptor& describe() noexcept;
        [[nodiscard]] static RenderResult<std::size_t> wireSize(const ViewRequest&) noexcept { return 16; }
        [[nodiscard]] static RenderResult<void> encode(const ViewRequest& value, cxx::BinarySpanWriter& out) noexcept;
        [[nodiscard]] static RenderResult<ViewRequest> decode(cxx::BinaryReader& in, BorrowedWireView) noexcept;
        [[nodiscard]] static RenderResult<ViewRequest> decode(cxx::BinaryReader& in, WireView) noexcept;
    };

    template <> struct DataSchema<CreateTargetRequest>
    {
        [[nodiscard]] static const DataSchemaDescriptor& describe() noexcept;
        [[nodiscard]] static RenderResult<std::size_t> wireSize(const CreateTargetRequest&) noexcept { return 20; }
        [[nodiscard]] static RenderResult<void>
        encode(const CreateTargetRequest& value, cxx::BinarySpanWriter& out) noexcept;
        [[nodiscard]] static RenderResult<CreateTargetRequest> decode(cxx::BinaryReader& in, BorrowedWireView) noexcept;
        [[nodiscard]] static RenderResult<CreateTargetRequest> decode(cxx::BinaryReader& in, WireView) noexcept;
    };

    template <> struct DataSchema<ResizeTargetRequest>
    {
        [[nodiscard]] static const DataSchemaDescriptor& describe() noexcept;
        [[nodiscard]] static RenderResult<std::size_t> wireSize(const ResizeTargetRequest&) noexcept { return 28; }
        [[nodiscard]] static RenderResult<void>
        encode(const ResizeTargetRequest& value, cxx::BinarySpanWriter& out) noexcept;
        [[nodiscard]] static RenderResult<ResizeTargetRequest> decode(cxx::BinaryReader& in, BorrowedWireView) noexcept;
        [[nodiscard]] static RenderResult<ResizeTargetRequest> decode(cxx::BinaryReader& in, WireView) noexcept;
    };

    template <> struct DataSchema<EffectRequest>
    {
        [[nodiscard]] static const DataSchemaDescriptor& describe() noexcept;
        [[nodiscard]] static RenderResult<std::size_t> wireSize(const EffectRequest&) noexcept { return 8; }
        [[nodiscard]] static RenderResult<void> encode(const EffectRequest& value, cxx::BinarySpanWriter& out) noexcept;
        [[nodiscard]] static RenderResult<EffectRequest> decode(cxx::BinaryReader& in, BorrowedWireView) noexcept;
        [[nodiscard]] static RenderResult<EffectRequest> decode(cxx::BinaryReader& in, WireView) noexcept;
    };

    template <> struct DataSchema<AttachmentRequest>
    {
        [[nodiscard]] static const DataSchemaDescriptor& describe() noexcept;
        [[nodiscard]] static RenderResult<std::size_t> wireSize(const AttachmentRequest&) noexcept { return 32; }
        [[nodiscard]] static RenderResult<void>
        encode(const AttachmentRequest& value, cxx::BinarySpanWriter& out) noexcept;
        [[nodiscard]] static RenderResult<AttachmentRequest> decode(cxx::BinaryReader& in, BorrowedWireView) noexcept;
        [[nodiscard]] static RenderResult<AttachmentRequest> decode(cxx::BinaryReader& in, WireView) noexcept;
    };

    template <> struct DataSchema<SceneCreated>
    {
        [[nodiscard]] static const DataSchemaDescriptor& describe() noexcept;
        [[nodiscard]] static RenderResult<std::size_t> wireSize(const SceneCreated&) noexcept { return 8; }
        [[nodiscard]] static RenderResult<void> encode(const SceneCreated& value, cxx::BinarySpanWriter& out) noexcept;
        [[nodiscard]] static RenderResult<SceneCreated> decode(cxx::BinaryReader& in, BorrowedWireView) noexcept;
        [[nodiscard]] static RenderResult<SceneCreated> decode(cxx::BinaryReader& in, WireView) noexcept;
    };

    template <> struct DataSchema<ViewCreated>
    {
        [[nodiscard]] static const DataSchemaDescriptor& describe() noexcept;
        [[nodiscard]] static RenderResult<std::size_t> wireSize(const ViewCreated&) noexcept { return 8; }
        [[nodiscard]] static RenderResult<void> encode(const ViewCreated& value, cxx::BinarySpanWriter& out) noexcept;
        [[nodiscard]] static RenderResult<ViewCreated> decode(cxx::BinaryReader& in, BorrowedWireView) noexcept;
        [[nodiscard]] static RenderResult<ViewCreated> decode(cxx::BinaryReader& in, WireView) noexcept;
    };

    template <> struct DataSchema<TargetCreated>
    {
        [[nodiscard]] static const DataSchemaDescriptor& describe() noexcept;
        [[nodiscard]] static RenderResult<std::size_t> wireSize(const TargetCreated&) noexcept { return 16; }
        [[nodiscard]] static RenderResult<void> encode(const TargetCreated& value, cxx::BinarySpanWriter& out) noexcept;
        [[nodiscard]] static RenderResult<TargetCreated> decode(cxx::BinaryReader& in, BorrowedWireView) noexcept;
        [[nodiscard]] static RenderResult<TargetCreated> decode(cxx::BinaryReader& in, WireView) noexcept;
    };

    template <> struct DataSchema<EffectCreated>
    {
        [[nodiscard]] static const DataSchemaDescriptor& describe() noexcept;
        [[nodiscard]] static RenderResult<std::size_t> wireSize(const EffectCreated&) noexcept { return 8; }
        [[nodiscard]] static RenderResult<void> encode(const EffectCreated& value, cxx::BinarySpanWriter& out) noexcept;
        [[nodiscard]] static RenderResult<EffectCreated> decode(cxx::BinaryReader& in, BorrowedWireView) noexcept;
        [[nodiscard]] static RenderResult<EffectCreated> decode(cxx::BinaryReader& in, WireView) noexcept;
    };

    template <> struct DataSchema<ReadbackResult>
    {
        [[nodiscard]] static const DataSchemaDescriptor& describe() noexcept;
        [[nodiscard]] static RenderResult<std::size_t> wireSize(const ReadbackResult&) noexcept { return 20; }
        [[nodiscard]] static RenderResult<void>
        encode(const ReadbackResult& value, cxx::BinarySpanWriter& out) noexcept;
        [[nodiscard]] static RenderResult<ReadbackResult> decode(cxx::BinaryReader& in, BorrowedWireView) noexcept;
        [[nodiscard]] static RenderResult<ReadbackResult> decode(cxx::BinaryReader& in, WireView) noexcept;
    };

    struct CreateEffectRequest final
    {
        TargetDesc description;
        OwnedWireBytes selections;
    };

    template <> struct DataSchema<CreateEffectRequest>
    {
        [[nodiscard]] static const DataSchemaDescriptor& describe() noexcept;
        [[nodiscard]] static RenderResult<std::size_t> wireSize(const CreateEffectRequest& value) noexcept;
        [[nodiscard]] static RenderResult<void>
        encode(const CreateEffectRequest& value, cxx::BinarySpanWriter& out) noexcept;
        [[nodiscard]] static RenderResult<CreateEffectRequest> decode(cxx::BinaryReader& in, WireView wire) noexcept;
    };

    [[nodiscard]] RenderResult<OwnedWireBytes>
    encodeFeatureSelections(std::span<const FeatureSelection> selected) noexcept;

    namespace operations
    {
        [[nodiscard]] const OperationDescriptor& createScene() noexcept;
        [[nodiscard]] const OperationDescriptor& removeScene() noexcept;
        [[nodiscard]] const OperationDescriptor& createView() noexcept;
        [[nodiscard]] const OperationDescriptor& removeView() noexcept;
        [[nodiscard]] const OperationDescriptor& createTarget() noexcept;
        [[nodiscard]] const OperationDescriptor& resizeTarget() noexcept;
        [[nodiscard]] const OperationDescriptor& createEffect() noexcept;
        [[nodiscard]] const OperationDescriptor& removeEffect() noexcept;
        [[nodiscard]] const OperationDescriptor& attach() noexcept;
        [[nodiscard]] const OperationDescriptor& detach() noexcept;
        [[nodiscard]] const OperationDescriptor& readback() noexcept;
        [[nodiscard]] const OperationDescriptor& stop() noexcept;
    }
}
