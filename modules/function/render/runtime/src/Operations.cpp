#include <lux/engine/render/runtime/Operations.hpp>
#include "Wire.hpp"

namespace lux::render
{
    const DataSchemaDescriptor& DataSchema<EmptyRequest>::describe() noexcept
    {
        static constexpr std::uint64_t zero[]{0};
        static constexpr DataField fields[]{
            {"reserved", cxx::EScalarKind::U32, 1, EDataFieldKind::SCALAR, 1, zero},
        };
        static const DataSchemaDescriptor schema{
            DataTypeIdView("lux.render.EmptyRequest"), {1, 0}, {1, 0}, fields, 4, 4
        };
        return schema;
    }

    RenderResult<void> DataSchema<EmptyRequest>::encode(const EmptyRequest& value, cxx::BinarySpanWriter& out) noexcept
    {
        out.writeUnsigned(value.reserved);
        return {};
    }

    RenderResult<EmptyRequest> DataSchema<EmptyRequest>::decode(cxx::BinaryReader& in, BorrowedWireView) noexcept
    {
        EmptyRequest value;
        in.readUnsigned(value.reserved);
        return value;
    }

    RenderResult<EmptyRequest> DataSchema<EmptyRequest>::decode(cxx::BinaryReader& in, WireView wire) noexcept
    {
        return decode(in, BorrowedWireView{wire.schema, wire.bytes.view(), {}});
    }

    const DataSchemaDescriptor& DataSchema<SceneRequest>::describe() noexcept
    {
        static constexpr DataField fields[]{
            {"scene", cxx::EScalarKind::U32, 2},
        };
        static const DataSchemaDescriptor schema{
            DataTypeIdView("lux.render.SceneRequest"), {1, 0}, {1, 0}, fields, 8, 4
        };
        return schema;
    }

    RenderResult<void> DataSchema<SceneRequest>::encode(const SceneRequest& value, cxx::BinarySpanWriter& out) noexcept
    {
        out.writeUnsigned(value.scene.index);
        out.writeUnsigned(value.scene.gen);
        return {};
    }

    RenderResult<SceneRequest> DataSchema<SceneRequest>::decode(cxx::BinaryReader& in, BorrowedWireView) noexcept
    {
        SceneRequest value;
        in.readUnsigned(value.scene.index);
        in.readUnsigned(value.scene.gen);
        return value;
    }

    RenderResult<SceneRequest> DataSchema<SceneRequest>::decode(cxx::BinaryReader& in, WireView wire) noexcept
    {
        return decode(in, BorrowedWireView{wire.schema, wire.bytes.view(), {}});
    }

    const DataSchemaDescriptor& DataSchema<CreateViewRequest>::describe() noexcept
    {
        static constexpr DataField fields[]{
            {"scene", cxx::EScalarKind::U32, 2},
            {"color", cxx::EScalarKind::F32, 4},
        };
        static const DataSchemaDescriptor schema{
            DataTypeIdView("lux.render.CreateViewRequest"), {1, 0}, {1, 0}, fields, 24, 4
        };
        return schema;
    }

    RenderResult<void>
    DataSchema<CreateViewRequest>::encode(const CreateViewRequest& value, cxx::BinarySpanWriter& out) noexcept
    {
        out.writeUnsigned(value.scene.index);
        out.writeUnsigned(value.scene.gen);
        for (auto x : value.color) out.writeFloat(x);
        return {};
    }

    RenderResult<CreateViewRequest>
    DataSchema<CreateViewRequest>::decode(cxx::BinaryReader& in, BorrowedWireView) noexcept
    {
        CreateViewRequest value;
        in.readUnsigned(value.scene.index);
        in.readUnsigned(value.scene.gen);
        for (auto& x : value.color) in.readFloat(x);
        return value;
    }

    RenderResult<CreateViewRequest> DataSchema<CreateViewRequest>::decode(cxx::BinaryReader& in, WireView wire) noexcept
    {
        return decode(in, BorrowedWireView{wire.schema, wire.bytes.view(), {}});
    }

    const DataSchemaDescriptor& DataSchema<ViewRequest>::describe() noexcept
    {
        static constexpr DataField fields[]{
            {"scene", cxx::EScalarKind::U32, 2},
            {"view", cxx::EScalarKind::U32, 2},
        };
        static const DataSchemaDescriptor schema{
            DataTypeIdView("lux.render.ViewRequest"), {1, 0}, {1, 0}, fields, 16, 4
        };
        return schema;
    }

    RenderResult<void> DataSchema<ViewRequest>::encode(const ViewRequest& value, cxx::BinarySpanWriter& out) noexcept
    {
        out.writeUnsigned(value.scene.index);
        out.writeUnsigned(value.scene.gen);
        out.writeUnsigned(value.view.index);
        out.writeUnsigned(value.view.gen);
        return {};
    }

    RenderResult<ViewRequest> DataSchema<ViewRequest>::decode(cxx::BinaryReader& in, BorrowedWireView) noexcept
    {
        ViewRequest value;
        in.readUnsigned(value.scene.index);
        in.readUnsigned(value.scene.gen);
        in.readUnsigned(value.view.index);
        in.readUnsigned(value.view.gen);
        return value;
    }

    RenderResult<ViewRequest> DataSchema<ViewRequest>::decode(cxx::BinaryReader& in, WireView wire) noexcept
    {
        return decode(in, BorrowedWireView{wire.schema, wire.bytes.view(), {}});
    }

    const DataSchemaDescriptor& DataSchema<CreateTargetRequest>::describe() noexcept
    {
        static constexpr DataField fields[]{
            {"description", cxx::EScalarKind::U32, 5},
        };
        static const DataSchemaDescriptor schema{
            DataTypeIdView("lux.render.CreateTargetRequest"), {1, 0}, {1, 0}, fields, 20, 4
        };
        return schema;
    }

    RenderResult<void>
    DataSchema<CreateTargetRequest>::encode(const CreateTargetRequest& value, cxx::BinarySpanWriter& out) noexcept
    {
        out.writeUnsigned(value.description.width);
        out.writeUnsigned(value.description.height);
        out.writeUnsigned(static_cast<std::uint32_t>(value.description.format));
        out.writeUnsigned(value.description.samples);
        out.writeUnsigned(static_cast<std::uint32_t>(value.description.kind));
        return {};
    }

    RenderResult<CreateTargetRequest>
    DataSchema<CreateTargetRequest>::decode(cxx::BinaryReader& in, BorrowedWireView) noexcept
    {
        CreateTargetRequest value;
        std::uint32_t format = 0, kind = 0;
        in.readUnsigned(value.description.width);
        in.readUnsigned(value.description.height);
        in.readUnsigned(format);
        in.readUnsigned(value.description.samples);
        in.readUnsigned(kind);
        value.description.format = static_cast<rdesc::ETextureFormat>(format);
        value.description.kind = static_cast<ETargetKind>(kind);
        return value;
    }

    RenderResult<CreateTargetRequest>
    DataSchema<CreateTargetRequest>::decode(cxx::BinaryReader& in, WireView wire) noexcept
    {
        return decode(in, BorrowedWireView{wire.schema, wire.bytes.view(), {}});
    }

    const DataSchemaDescriptor& DataSchema<ResizeTargetRequest>::describe() noexcept
    {
        static constexpr DataField fields[]{
            {"target", cxx::EScalarKind::U32, 2},
            {"description", cxx::EScalarKind::U32, 5},
        };
        static const DataSchemaDescriptor schema{
            DataTypeIdView("lux.render.ResizeTargetRequest"), {1, 0}, {1, 0}, fields, 28, 4
        };
        return schema;
    }

    RenderResult<void>
    DataSchema<ResizeTargetRequest>::encode(const ResizeTargetRequest& value, cxx::BinarySpanWriter& out) noexcept
    {
        out.writeUnsigned(value.target.index);
        out.writeUnsigned(value.target.gen);
        out.writeUnsigned(value.description.width);
        out.writeUnsigned(value.description.height);
        out.writeUnsigned(static_cast<std::uint32_t>(value.description.format));
        out.writeUnsigned(value.description.samples);
        out.writeUnsigned(static_cast<std::uint32_t>(value.description.kind));
        return {};
    }

    RenderResult<ResizeTargetRequest>
    DataSchema<ResizeTargetRequest>::decode(cxx::BinaryReader& in, BorrowedWireView) noexcept
    {
        ResizeTargetRequest value;
        in.readUnsigned(value.target.index);
        in.readUnsigned(value.target.gen);
        std::uint32_t format = 0, kind = 0;
        in.readUnsigned(value.description.width);
        in.readUnsigned(value.description.height);
        in.readUnsigned(format);
        in.readUnsigned(value.description.samples);
        in.readUnsigned(kind);
        value.description.format = static_cast<rdesc::ETextureFormat>(format);
        value.description.kind = static_cast<ETargetKind>(kind);
        return value;
    }

    RenderResult<ResizeTargetRequest>
    DataSchema<ResizeTargetRequest>::decode(cxx::BinaryReader& in, WireView wire) noexcept
    {
        return decode(in, BorrowedWireView{wire.schema, wire.bytes.view(), {}});
    }

    const DataSchemaDescriptor& DataSchema<EffectRequest>::describe() noexcept
    {
        static constexpr DataField fields[]{
            {"effect", cxx::EScalarKind::U32, 2},
        };
        static const DataSchemaDescriptor schema{
            DataTypeIdView("lux.render.EffectRequest"), {1, 0}, {1, 0}, fields, 8, 4
        };
        return schema;
    }

    RenderResult<void>
    DataSchema<EffectRequest>::encode(const EffectRequest& value, cxx::BinarySpanWriter& out) noexcept
    {
        out.writeUnsigned(value.effect.index);
        out.writeUnsigned(value.effect.gen);
        return {};
    }

    RenderResult<EffectRequest> DataSchema<EffectRequest>::decode(cxx::BinaryReader& in, BorrowedWireView) noexcept
    {
        EffectRequest value;
        in.readUnsigned(value.effect.index);
        in.readUnsigned(value.effect.gen);
        return value;
    }

    RenderResult<EffectRequest> DataSchema<EffectRequest>::decode(cxx::BinaryReader& in, WireView wire) noexcept
    {
        return decode(in, BorrowedWireView{wire.schema, wire.bytes.view(), {}});
    }

    const DataSchemaDescriptor& DataSchema<AttachmentRequest>::describe() noexcept
    {
        static constexpr DataField fields[]{
            {"scene", cxx::EScalarKind::U32, 2},
            {"view", cxx::EScalarKind::U32, 2},
            {"target", cxx::EScalarKind::U32, 2},
            {"effect", cxx::EScalarKind::U32, 2},
        };
        static const DataSchemaDescriptor schema{
            DataTypeIdView("lux.render.AttachmentRequest"), {1, 0}, {1, 0}, fields, 32, 4
        };
        return schema;
    }

    RenderResult<void>
    DataSchema<AttachmentRequest>::encode(const AttachmentRequest& value, cxx::BinarySpanWriter& out) noexcept
    {
        out.writeUnsigned(value.scene.index);
        out.writeUnsigned(value.scene.gen);
        out.writeUnsigned(value.view.index);
        out.writeUnsigned(value.view.gen);
        out.writeUnsigned(value.target.index);
        out.writeUnsigned(value.target.gen);
        out.writeUnsigned(value.effect.index);
        out.writeUnsigned(value.effect.gen);
        return {};
    }

    RenderResult<AttachmentRequest>
    DataSchema<AttachmentRequest>::decode(cxx::BinaryReader& in, BorrowedWireView) noexcept
    {
        AttachmentRequest value;
        in.readUnsigned(value.scene.index);
        in.readUnsigned(value.scene.gen);
        in.readUnsigned(value.view.index);
        in.readUnsigned(value.view.gen);
        in.readUnsigned(value.target.index);
        in.readUnsigned(value.target.gen);
        in.readUnsigned(value.effect.index);
        in.readUnsigned(value.effect.gen);
        return value;
    }

    RenderResult<AttachmentRequest> DataSchema<AttachmentRequest>::decode(cxx::BinaryReader& in, WireView wire) noexcept
    {
        return decode(in, BorrowedWireView{wire.schema, wire.bytes.view(), {}});
    }

    const DataSchemaDescriptor& DataSchema<SceneCreated>::describe() noexcept
    {
        static constexpr DataField fields[]{
            {"scene", cxx::EScalarKind::U32, 2},
        };
        static const DataSchemaDescriptor schema{
            DataTypeIdView("lux.render.SceneCreated"), {1, 0}, {1, 0}, fields, 8, 4
        };
        return schema;
    }

    RenderResult<void> DataSchema<SceneCreated>::encode(const SceneCreated& value, cxx::BinarySpanWriter& out) noexcept
    {
        out.writeUnsigned(value.scene.index);
        out.writeUnsigned(value.scene.gen);
        return {};
    }

    RenderResult<SceneCreated> DataSchema<SceneCreated>::decode(cxx::BinaryReader& in, BorrowedWireView) noexcept
    {
        SceneCreated value;
        in.readUnsigned(value.scene.index);
        in.readUnsigned(value.scene.gen);
        return value;
    }

    RenderResult<SceneCreated> DataSchema<SceneCreated>::decode(cxx::BinaryReader& in, WireView wire) noexcept
    {
        return decode(in, BorrowedWireView{wire.schema, wire.bytes.view(), {}});
    }

    const DataSchemaDescriptor& DataSchema<ViewCreated>::describe() noexcept
    {
        static constexpr DataField fields[]{
            {"view", cxx::EScalarKind::U32, 2},
        };
        static const DataSchemaDescriptor schema{
            DataTypeIdView("lux.render.ViewCreated"), {1, 0}, {1, 0}, fields, 8, 4
        };
        return schema;
    }

    RenderResult<void> DataSchema<ViewCreated>::encode(const ViewCreated& value, cxx::BinarySpanWriter& out) noexcept
    {
        out.writeUnsigned(value.view.index);
        out.writeUnsigned(value.view.gen);
        return {};
    }

    RenderResult<ViewCreated> DataSchema<ViewCreated>::decode(cxx::BinaryReader& in, BorrowedWireView) noexcept
    {
        ViewCreated value;
        in.readUnsigned(value.view.index);
        in.readUnsigned(value.view.gen);
        return value;
    }

    RenderResult<ViewCreated> DataSchema<ViewCreated>::decode(cxx::BinaryReader& in, WireView wire) noexcept
    {
        return decode(in, BorrowedWireView{wire.schema, wire.bytes.view(), {}});
    }

    const DataSchemaDescriptor& DataSchema<TargetCreated>::describe() noexcept
    {
        static constexpr DataField fields[]{
            {"target", cxx::EScalarKind::U32, 2},
            {"epoch", cxx::EScalarKind::U64, 1},
        };
        static const DataSchemaDescriptor schema{
            DataTypeIdView("lux.render.TargetCreated"), {1, 0}, {1, 0}, fields, 16, 4
        };
        return schema;
    }

    RenderResult<void>
    DataSchema<TargetCreated>::encode(const TargetCreated& value, cxx::BinarySpanWriter& out) noexcept
    {
        out.writeUnsigned(value.target.index);
        out.writeUnsigned(value.target.gen);
        out.writeUnsigned(value.epoch);
        return {};
    }

    RenderResult<TargetCreated> DataSchema<TargetCreated>::decode(cxx::BinaryReader& in, BorrowedWireView) noexcept
    {
        TargetCreated value;
        in.readUnsigned(value.target.index);
        in.readUnsigned(value.target.gen);
        in.readUnsigned(value.epoch);
        return value;
    }

    RenderResult<TargetCreated> DataSchema<TargetCreated>::decode(cxx::BinaryReader& in, WireView wire) noexcept
    {
        return decode(in, BorrowedWireView{wire.schema, wire.bytes.view(), {}});
    }

    const DataSchemaDescriptor& DataSchema<EffectCreated>::describe() noexcept
    {
        static constexpr DataField fields[]{
            {"effect", cxx::EScalarKind::U32, 2},
        };
        static const DataSchemaDescriptor schema{
            DataTypeIdView("lux.render.EffectCreated"), {1, 0}, {1, 0}, fields, 8, 4
        };
        return schema;
    }

    RenderResult<void>
    DataSchema<EffectCreated>::encode(const EffectCreated& value, cxx::BinarySpanWriter& out) noexcept
    {
        out.writeUnsigned(value.effect.index);
        out.writeUnsigned(value.effect.gen);
        return {};
    }

    RenderResult<EffectCreated> DataSchema<EffectCreated>::decode(cxx::BinaryReader& in, BorrowedWireView) noexcept
    {
        EffectCreated value;
        in.readUnsigned(value.effect.index);
        in.readUnsigned(value.effect.gen);
        return value;
    }

    RenderResult<EffectCreated> DataSchema<EffectCreated>::decode(cxx::BinaryReader& in, WireView wire) noexcept
    {
        return decode(in, BorrowedWireView{wire.schema, wire.bytes.view(), {}});
    }

    const DataSchemaDescriptor& DataSchema<ReadbackResult>::describe() noexcept
    {
        static constexpr DataField fields[]{
            {"pixels", cxx::EScalarKind::U8, 1, EDataFieldKind::EXTERNAL},
        };
        static const DataSchemaDescriptor schema{
            DataTypeIdView("lux.render.ReadbackResult"), {1, 0}, {1, 0}, fields, 20, 4
        };
        return schema;
    }

    RenderResult<void>
    DataSchema<ReadbackResult>::encode(const ReadbackResult& value, cxx::BinarySpanWriter& out) noexcept
    {
        out.writeUnsigned(value.pixels.attachment_index);
        out.writeUnsigned(value.pixels.offset);
        out.writeUnsigned(value.pixels.size);
        return {};
    }

    RenderResult<ReadbackResult> DataSchema<ReadbackResult>::decode(cxx::BinaryReader& in, BorrowedWireView) noexcept
    {
        ReadbackResult value;
        in.readUnsigned(value.pixels.attachment_index);
        in.readUnsigned(value.pixels.offset);
        in.readUnsigned(value.pixels.size);
        return value;
    }

    RenderResult<ReadbackResult> DataSchema<ReadbackResult>::decode(cxx::BinaryReader& in, WireView wire) noexcept
    {
        return decode(in, BorrowedWireView{wire.schema, wire.bytes.view(), {}});
    }

    const OperationDescriptor& operations::createScene() noexcept
    {
        static const auto value = operationDescriptor<EmptyRequest, SceneCreated>("render.scene.create");
        return value;
    }

    const OperationDescriptor& operations::removeScene() noexcept
    {
        static const auto value = operationDescriptor<SceneRequest>("render.scene.remove");
        return value;
    }

    const OperationDescriptor& operations::createView() noexcept
    {
        static const auto value = operationDescriptor<CreateViewRequest, ViewCreated>("render.view.create");
        return value;
    }

    const OperationDescriptor& operations::removeView() noexcept
    {
        static const auto value = operationDescriptor<ViewRequest>("render.view.remove");
        return value;
    }

    const OperationDescriptor& operations::createTarget() noexcept
    {
        static const auto value = operationDescriptor<CreateTargetRequest, TargetCreated>("render.target.create");
        return value;
    }

    const OperationDescriptor& operations::resizeTarget() noexcept
    {
        static const auto value = operationDescriptor<ResizeTargetRequest, TargetCreated>("render.target.resize");
        return value;
    }

    const OperationDescriptor& operations::createEffect() noexcept
    {
        static const auto value = operationDescriptor<CreateEffectRequest, EffectCreated>("render.effect.create");
        return value;
    }

    const OperationDescriptor& operations::removeEffect() noexcept
    {
        static const auto value = operationDescriptor<EffectRequest>("render.effect.remove");
        return value;
    }

    const OperationDescriptor& operations::attach() noexcept
    {
        static const auto value = operationDescriptor<AttachmentRequest>("render.attach");
        return value;
    }

    const OperationDescriptor& operations::detach() noexcept
    {
        static const auto value = operationDescriptor<AttachmentRequest>("render.detach");
        return value;
    }

    const OperationDescriptor& operations::readback() noexcept
    {
        static const auto value = operationDescriptor<AttachmentRequest, ReadbackResult>(
            "render.readback", {1, 0}, ECompletionContract::RESULT_AVAILABLE
        );
        return value;
    }

    const OperationDescriptor& operations::stop() noexcept
    {
        static const auto value = operationDescriptor<EmptyRequest>("render.stop");
        return value;
    }


    const DataSchemaDescriptor& DataSchema<CreateEffectRequest>::describe() noexcept
    {
        static constexpr DataField fields[]{
            {"target", cxx::EScalarKind::U32, 5},
            {"selections", cxx::EScalarKind::U8, 1, EDataFieldKind::BLOB}
        };
        static const DataSchemaDescriptor schema{
            DataTypeIdView("lux.render.CreateEffectRequest"), {1, 0}, {1, 0}, fields, 36, 4
        };
        return schema;
    }

    RenderResult<std::size_t> DataSchema<CreateEffectRequest>::wireSize(const CreateEffectRequest& value) noexcept
    {
        if (value.selections.size() > runtime_detail::wire_limits.max_wire_bytes - 36)
            return cxx::unexpected(RenderError{kWireLimit});
        return 36 + value.selections.size();
    }

    RenderResult<void> DataSchema<CreateEffectRequest>::encode(const CreateEffectRequest& value,
        cxx::BinarySpanWriter& out) noexcept
    {
        auto result = DataSchema<CreateTargetRequest>::encode({value.description}, out);
        out.writeUnsigned<std::uint64_t>(36);
        out.writeUnsigned<std::uint64_t>(value.selections.size());
        out.writeBytes(value.selections.view());
        return result;
    }

    RenderResult<CreateEffectRequest> DataSchema<CreateEffectRequest>::decode(cxx::BinaryReader& in,
        WireView wire) noexcept
    {
        auto target = DataSchema<CreateTargetRequest>::decode(in, wire);
        BlobRef ref;
        in.readUnsigned(ref.offset);
        in.readUnsigned(ref.size);
        auto bytes = resolveBlob(wire.bytes, ref, runtime_detail::wire_limits.max_wire_bytes);
        if (!bytes) return cxx::unexpected(bytes.error());
        return CreateEffectRequest{target->description, std::move(*bytes)};
    }

    RenderResult<OwnedWireBytes> encodeFeatureSelections(std::span<const FeatureSelection> selected) noexcept
    {
        auto encoded = runtime_detail::selections(selected);
        if (!encoded) return cxx::unexpected(encoded.error());
        return runtime_detail::own(std::move(*encoded));
    }
}
