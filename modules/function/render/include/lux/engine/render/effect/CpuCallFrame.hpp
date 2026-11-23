#pragma once

#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/render/core/Resource.hpp>
#include <lux/engine/render/graph/DrawList.hpp>
#include <lux/engine/render/graph/DrawResource.hpp>
#include <lux/engine/render/graph/ConstantBlock.hpp>
#include <lux/engine/render/extensions/graph/GraphError.hpp>
#include <lux/engine/render/graph/IndexedGeometry.hpp>
#include <array>
#include <bit>
#include <concepts>
#include <type_traits>
#include <functional>
#include <span>

namespace lux::rdesc { struct ShaderBlock; }

namespace lux::render
{
    namespace detail { class JobCpuPreparation; struct CpuCall; }

    template <class T>
    concept CpuPreparationValue = std::is_trivially_copyable_v<T> && std::same_as<T, std::remove_cv_t<T>> &&
        !std::is_array_v<T>;

    class InstanceLinkContext;

    struct GraphCpuDataId final
    {
        std::uint32_t index = kGraphInvalidIndex;
        bool operator==(const GraphCpuDataId&) const noexcept = default;
    };

    template <CpuPreparationValue T> class CpuInputSlot final
    {
        friend class CpuCallFrame;
        friend class InstanceLinkContext;
        explicit constexpr CpuInputSlot(std::uint32_t slot) noexcept : slot_(slot) {}
        std::uint32_t slot_;
    };

    template <ShaderConstantBlock T> class InlineConstantWriter final
    {
        friend class CpuCallFrame;
        friend class InstanceLinkContext;
        explicit constexpr InlineConstantWriter(std::uint32_t slot) noexcept : slot_(slot) {}
        std::uint32_t slot_;
    };

    class CpuCallFrame;

    struct RasterRecordLayout final
    {
        const rdesc::ShaderBlock& block;
        std::uint32_t frame_slot;
        bool reads_images;
    };

    namespace extension_detail
    {
        struct RasterRecordDestination final
        {
            DrawSourceAddress source;
            std::uint32_t list, program;
            RasterRecordLayout layout;
        };
    }

    // Borrows this preparation and its selected Program until prepare() returns.
    class RasterRecordWriter final
    {
    public:
        [[nodiscard]] const RasterRecordLayout& layout() const & noexcept { return destination_.layout; }
        const RasterRecordLayout& layout() const && = delete;

        [[nodiscard]] RenderResult<void> write(const RasterRecordInput& record) const noexcept;

        template <class Storage>
        [[nodiscard]] RenderResult<void>
        write(const RasterRecordInput& record, DrawResourceSlot<Storage> images) const noexcept;

    private:
        friend class CpuCallFrame;

        RasterRecordWriter(CpuCallFrame& frame, extension_detail::RasterRecordDestination destination) noexcept
            : frame_(frame), destination_(destination) {}

        CpuCallFrame& frame_;
        extension_detail::RasterRecordDestination destination_;
    };

    template <class T>
    concept CpuFrameInput = CpuPreparationValue<T> ||
        (std::is_object_v<std::remove_reference_t<T>> && std::same_as<T, const std::remove_cvref_t<T>&>);

    template <CpuFrameInput... T> struct CpuFrameInputs final
    {
        CpuCallFrame& frame;
        // Const references borrow captured Resource snapshots. Values come from upstream CPU calls.
        std::tuple<RevisionedValue<std::conditional_t<std::is_reference_v<T>,
            std::reference_wrapper<const std::remove_reference_t<T>>, T>>...> values;
    };

    namespace extension_detail
    {
        template <class T>
        RenderResult<void> prepareCpuCall(const void*, CpuCallFrame&, void*, const void*) noexcept;
    }

    // Valid only for the current CPU call on this Job and frame slot.
    class LUX_RENDER_PUBLIC CpuCallFrame final
    {
    public:
        CpuCallFrame(const CpuCallFrame&) = delete;
        CpuCallFrame& operator=(const CpuCallFrame&) = delete;
        [[nodiscard]] const TargetDesc& target() const noexcept { return target_; }
        [[nodiscard]] RenderResult<void> setRasterStream(DrawStreamSlot, const RasterStreamData&) noexcept;
        [[nodiscard]] RenderResult<RasterProgramSlot>
        rasterProgram(DrawStreamSlot, std::uint32_t selection_index) const noexcept;
        template <class Storage>
        [[nodiscard]] RenderResult<RasterRecordWriter>
        rasterRecord(DrawResourceSlot<Storage> source, RasterProgramSlot program) noexcept
        {
            auto destination = recordDestination(source.address_, program);
            if (!destination) return cxx::unexpected(destination.error());
            return RasterRecordWriter{*this, *destination};
        }

        // Borrow the installed Resource only during this preparation. An absent optional source returns null.
        template <class Storage>
        [[nodiscard]] RenderResult<const Storage*> readDrawResource(DrawResourceSlot<Storage> slot) const noexcept
        {
            const auto selected = readDrawSource(slot.address_, cxx::make_basic_type_info<Storage>());
            if (!selected) return cxx::unexpected(selected.error());
            return static_cast<const Storage*>(*selected);
        }

        template <class Storage>
        [[nodiscard]] RenderResult<RasterIndexedGeometry>
        bindIndexedGeometry(DrawResourceSlot<Storage> source, const IndexedGeometryInput& geometry) noexcept
        {
            return appendIndexedGeometry(source.address_, geometry);
        }

        template <class Storage>
        [[nodiscard]] RenderResult<void>
        useSampledImage(DrawResourceSlot<Storage> source, const nri::Texture& image, GraphImageRange range) noexcept
        {
            return appendSampledImage(source.address_, image, range);
        }

        template <CpuPreparationValue T>
        [[nodiscard]] RenderResult<RevisionedValue<T>> read(CpuInputSlot<T> slot) const noexcept
        {
            std::array<std::byte, sizeof(T)> bytes;
            auto revision = readBytes(slot.slot_, bytes);
            if (!revision) return cxx::unexpected(revision.error());
            return RevisionedValue<T>{std::bit_cast<T>(bytes), *revision};
        }

        // A nonzero revision identifies stable bytes from this writer's selected source.
        template <ShaderConstantBlock T>
        [[nodiscard]] RenderResult<void>
        write(InlineConstantWriter<T> writer, const T& value, std::uint64_t revision = 0) noexcept
        {
            return writeInlineBlock(writer.slot_, std::as_bytes(std::span{&value, 1}), revision);
        }

        // A nonzero revision identifies unchanged bytes from this writer across calls.
        [[nodiscard]] RenderResult<void>
        writeBuffer(std::size_t output, std::span<const std::byte>, std::uint64_t revision = 0) noexcept;
        [[nodiscard]] RenderResult<void> sizeBuffer(std::size_t output, std::uint64_t bytes) noexcept;

    private:
        friend class detail::JobCpuPreparation;
        CpuCallFrame(detail::JobCpuPreparation&, std::uint32_t call, std::uint32_t frame, const TargetDesc&) noexcept;

        friend class RasterRecordWriter;
        [[nodiscard]] RenderResult<extension_detail::RasterRecordDestination>
        recordDestination(extension_detail::DrawSourceAddress, RasterProgramSlot) const noexcept;
        [[nodiscard]] RenderResult<void>
        appendRasterRecord(
            const extension_detail::RasterRecordDestination&,
            const RasterRecordInput&,
            std::optional<extension_detail::DrawSourceAddress>
        ) noexcept;

        [[nodiscard]] RenderResult<RasterIndexedGeometry>
        appendIndexedGeometry(extension_detail::DrawSourceAddress, const IndexedGeometryInput&) noexcept;

        [[nodiscard]] RenderResult<void>
        appendSampledImage(extension_detail::DrawSourceAddress, const nri::Texture&, GraphImageRange) noexcept;
        [[nodiscard]] RenderResult<const void*>
        readDrawSource(extension_detail::DrawSourceAddress, cxx::basic_type_info) const noexcept;

        template <class K> friend RenderResult<void>
        extension_detail::prepareCpuCall(const void*, CpuCallFrame&, void*, const void*) noexcept;

        [[nodiscard]] RevisionedValue<const void*> snapshot(std::size_t ordinal) const noexcept;

        CpuCallFrame& arguments(std::type_identity<CpuCallFrame>) noexcept { return *this; }

        template <CpuFrameInput T> auto argument(std::size_t& value, std::size_t& source) const noexcept
        {
            if constexpr (std::is_reference_v<T>)
            {
                const auto selected = snapshot(source++);
                const auto& borrowed = *static_cast<std::remove_reference_t<T>*>(selected.value);
                return RevisionedValue{std::cref(borrowed), selected.revision};
            }
            else
            {
                std::array<std::byte, sizeof(T)> bytes;
                const auto revision = copyInput(value++, bytes);
                return RevisionedValue<T>{std::bit_cast<T>(bytes), revision};
            }
        }

        template <CpuFrameInput... T> CpuFrameInputs<T...> arguments(std::type_identity<CpuFrameInputs<T...>>) noexcept
        {
            std::size_t value = 0, source = 0;
            return {*this, {argument<T>(value, source)...}};
        }

        [[nodiscard]] std::uint64_t copyInput(std::size_t, std::span<std::byte>) const noexcept;
        void storeOutput(std::size_t, std::span<const std::byte>) noexcept;
        [[nodiscard]] RenderResult<std::uint64_t> readBytes(std::uint32_t, std::span<std::byte>) const noexcept;
        [[nodiscard]] RenderResult<void>
        writeInlineBlock(std::uint32_t, std::span<const std::byte>, std::uint64_t revision) noexcept;
        [[nodiscard]] RenderResult<std::uint32_t>
        drawList(std::uint32_t pass, std::optional<std::uint32_t> source = std::nullopt) const noexcept;
        [[nodiscard]] RenderResult<std::uint32_t> sampledImageList(extension_detail::DrawSourceAddress) const noexcept;
        [[nodiscard]] bool validSampledImage(const RasterSampledImageRead&) const noexcept;

        detail::JobCpuPreparation& job_;
        const detail::CpuCall& linked_;
        std::uint32_t call_slot_, frame_slot_;
        const TargetDesc& target_;
    };

    inline RenderResult<void> RasterRecordWriter::write(const RasterRecordInput& record) const noexcept
    {
        return frame_.appendRasterRecord(destination_, record, {});
    }

    template <class Storage>
    RenderResult<void>
    RasterRecordWriter::write(const RasterRecordInput& record, DrawResourceSlot<Storage> images) const noexcept
    {
        return frame_.appendRasterRecord(destination_, record, images.address_);
    }
} // namespace lux::render
