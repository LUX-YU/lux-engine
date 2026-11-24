#include <lux/engine/render/resource/RasterProgramSet.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/device/JobGpuResources.hpp>
#include <lux/engine/render/runtime/commands/Error.hpp>
#include <lux/engine/render/device/PreparedRasterTable.hpp>
#include <lux/engine/render/runtime/DeviceEntry.hpp>
#include <lux/engine/render/runtime/JobExecution.hpp>
#include <lux/cxx/core/scope_exit.hpp>
#include <bit>

namespace lux::render
{
    struct RasterProgramSet::Table final
    {
        explicit Table(detail::DeviceEntry& device) noexcept : native(device) {}
        detail::CompiledRasterStream::RuntimeContract contract;
        detail::PreparedRasterTable native;
    };

    namespace
    {
        RenderResult<nri::Format> attachmentFormat(
            const detail::DeviceSession& device, rdesc::ETextureFormat format, bool depth, std::uint32_t samples
        ) noexcept
        {
            const auto mapped = detail::imageFormat(format);
            const auto usage = depth ? rdesc::ETextureUsage::DEPTH_STENCIL_ATTACHMENT : rdesc::ETextureUsage::COLOR_ATTACHMENT;
            if (mapped.native == nri::Format::UNKNOWN || !rdesc::supportsTextureUsage(format, usage))
                return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            const auto native_usage = depth ? nri::TextureUsageBits::DEPTH_STENCIL_ATTACHMENT : nri::TextureUsageBits::COLOR_ATTACHMENT;
            const nri::TextureDesc desc{
                nri::TextureType::TEXTURE_2D, native_usage, mapped.native, 1, 1, 1, 1, 1,
                static_cast<nri::Sample_t>(samples), nri::SharingMode::EXCLUSIVE
            };
            if (auto valid = device.validateTexture(desc); !valid) return cxx::unexpected(valid.error());
            return mapped.native;
        }
    } // namespace

    RasterProgramSet::RasterProgramSet(ResourceCreateContext& context) noexcept : device_(context.device_owner_) {}
    RasterProgramSet::~RasterProgramSet() noexcept = default;

    exec::task<RenderResult<void>> RasterProgramSet::replace(Description description) noexcept
    {
        if (revision_ == UINT64_MAX) co_return cxx::unexpected(RenderError{kDeviceCapacity});
        if (preparing_) co_return cxx::unexpected(RenderError{kDeviceBusy});
        auto& device = device_.session();
        const bool invalid_samples = !std::has_single_bit(description.samples) || description.samples > UINT8_MAX;
        const bool too_many_colors = description.colors.size() > device.description().shaderStage.fragment.attachmentMaxNum;
        const bool has_depth = description.depth != rdesc::ETextureFormat::UNDEFINED;
        if (invalid_samples || too_many_colors || (description.section_depth_bias && !has_depth))
            co_return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
        if (description.shaders.size() >= kGraphInvalidIndex || description.programs.size() >= kGraphInvalidIndex)
            co_return cxx::unexpected(RenderError{kDeviceCapacity});

        auto candidate = std::make_unique<Table>(device_);
        auto& contract = candidate->contract;
        contract.arguments = std::move(description.arguments);
        contract.target = {
            {}, nri::Format::UNKNOWN, description.samples, description.section_depth_bias,
            description.shared_textures, description.shared_records
        };
        for (const auto color : description.colors)
        {
            const auto native = attachmentFormat(device, color, false, description.samples);
            if (!native) co_return cxx::unexpected(native.error());
            contract.target.colors.push_back(*native);
        }
        if (has_depth)
        {
            const auto native = attachmentFormat(device, description.depth, true, description.samples);
            if (!native) co_return cxx::unexpected(native.error());
            contract.target.depth = *native;
        }
        std::vector<std::uint32_t> remaining(description.shaders.size());
        for (const auto& program : description.programs)
        {
            if (program.program >= remaining.size()) co_return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            ++remaining[program.program];
        }
        std::vector<detail::RasterProgramLink> recipes;
        recipes.reserve(description.programs.size());
        for (const auto& program : description.programs)
        {
            auto& shader = description.shaders[program.program];
            auto linked = detail::linkRasterStreamProgram(device, {
                contract.arguments, program.parameters, contract.target,
                --remaining[program.program] ? shader : std::move(shader), program.graphics, program.geometry_locations
            });
            if (!linked) co_return cxx::unexpected(linked.error());
            recipes.push_back(std::move(*linked));
        }
        preparing_ = true;
        cxx::scope_exit finish([&]() noexcept { preparing_ = false; });
        if (auto compiled = co_await candidate->native.compile(std::move(recipes)); !compiled)
            co_return cxx::unexpected(compiled.error());
        current_ = std::move(candidate);
        ++revision_;
        co_return RenderResult<void>{};
    }

    RenderResult<void>
    runtime_detail::JobExecution::bindPublication(
        const detail::CompiledRasterStream& stream,
        const RasterProgramSet& set
    ) noexcept
    {
        if (&set.device_.session() != &gpu_.session) return cxx::unexpected(RenderError{kStorageMismatch});
        auto read = [&set, pass = stream.pass](const void*) noexcept -> RenderResult<detail::RasterProgramPublication>
        {
            const auto* published = set.current_.get();
            if (!published) return cxx::unexpected(RenderError{kFrameContract, {pass}});
            return detail::RasterProgramPublication{published->contract, published->native.programs(), set.revision_};
        };
        program_bindings_.push_back({&stream, std::move(read)});
        return {};
    }
} // namespace lux::render
