#include <NRI.h>

#include <Extensions/NRIHelper.h>
#include <lux/engine/render/device/DeviceBackend.hpp>
#include <lux/engine/render/device/DeviceBackendData.hpp>
#include <lux/engine/render/device/Error.hpp>

#include <algorithm>
#include <bit>

#ifdef _WIN32
#define NOMINMAX
#include <d3d12.h>
#undef interface
#endif

namespace lux::render::detail
{
    RenderResult<void>
    DeviceBackend::initializeD3D12(
        DeviceSession& session,
        const DeviceOptions& options,
        const nri::CallbackInterface& diagnostic,
        bool validation,
        std::uint32_t capacity
    ) noexcept
    {
#ifndef _WIN32
        return cxx::unexpected(RenderError{kDeviceUnsupported});
#else
        if (!capacity || !diagnostic.MessageCallback)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        std::uint32_t count = 0;
        auto status = nri::nriEnumerateAdapters(nullptr, count);
        if (status != nri::Result::SUCCESS)
        {
            return cxx::unexpected(nativeError(status));
        }
        if (!count)
        {
            return cxx::unexpected(RenderError{kDeviceUnsupported});
        }
        std::vector<nri::AdapterDesc> adapters(count);
        status = nri::nriEnumerateAdapters(adapters.data(), count);
        if (status != nri::Result::SUCCESS)
        {
            return cxx::unexpected(nativeError(status));
        }
        const auto selected = options.physical_device_index == UINT32_MAX ? 0U : options.physical_device_index;
        if (selected >= count)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        const auto& adapter = adapters[selected];
        std::array<nri::QueueType, 3> types{
            nri::QueueType::GRAPHICS,
            nri::QueueType::GRAPHICS,
            nri::QueueType::GRAPHICS
        };
        std::array<std::uint32_t, 3> indices{}, requested{1, 0, 0};
        if (options.multiple_queues)
        {
            for (unsigned role = 1; role < types.size(); ++role)
            {
                const auto dedicated = role == 1 ? nri::QueueType::COMPUTE : nri::QueueType::COPY;
                if (options.prefer_dedicated_queues && adapter.queueNum[static_cast<unsigned>(dedicated)])
                {
                    types[role] = dedicated;
                }
                const auto type = static_cast<unsigned>(types[role]);
                indices[role] = requested[type] < adapter.queueNum[type] ? requested[type]++ : 0;
            }
        }
        std::vector<nri::QueueFamilyDesc> families;
        for (unsigned type = 0; type < requested.size(); ++type)
        {
            if (requested[type])
            {
                families.push_back({nullptr, requested[type], static_cast<nri::QueueType>(type)});
            }
        }
        nri::DeviceCreationDesc creation{};
        creation.graphicsAPI = nri::GraphicsAPI::D3D12;
        creation.adapterDesc = &adapter;
        creation.queueFamilies = families.data();
        creation.queueFamilyNum = static_cast<std::uint32_t>(families.size());
        creation.callbackInterface = diagnostic;
        creation.enableNRIValidation = validation;
        creation.enableGraphicsAPIValidation = validation;
        HMODULE render_module{};
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&DeviceBackend::initializeD3D12),
                &render_module
            ))
        {
            return cxx::unexpected(RenderError{kDeviceFailure});
        }
        std::wstring sdk_path(32768, L'\0');
        const auto path_size = GetModuleFileNameW(render_module, sdk_path.data(), static_cast<DWORD>(sdk_path.size()));
        if (!path_size || path_size >= sdk_path.size())
        {
            return cxx::unexpected(RenderError{kDeviceFailure});
        }
        sdk_path.resize(path_size);
        const auto separator = sdk_path.find_last_of(L"\\/");
        if (separator == std::wstring::npos)
        {
            return cxx::unexpected(RenderError{kDeviceFailure});
        }
        sdk_path.resize(separator + 1);
        sdk_path += L"AgilitySDK\\";
        if (GetFileAttributesW((sdk_path + L"D3D12Core.dll").c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            return cxx::unexpected(RenderError{kDeviceFailure});
        }
        const auto utf8_size = WideCharToMultiByte(CP_UTF8, 0, sdk_path.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (!utf8_size)
        {
            return cxx::unexpected(RenderError{kDeviceFailure});
        }
        std::string sdk_utf8(static_cast<std::size_t>(utf8_size), '\0');
        if (!WideCharToMultiByte(CP_UTF8, 0, sdk_path.c_str(), -1, sdk_utf8.data(), utf8_size, nullptr, nullptr))
        {
            return cxx::unexpected(RenderError{kDeviceFailure});
        }
        creation.d3d12AgilitySdkPath = sdk_utf8.c_str();
        nri::Device* created = nullptr;
        status = nri::nriCreateDevice(creation, created);
        if (status != nri::Result::SUCCESS)
        {
            return cxx::unexpected(nativeError(status));
        }
        std::unique_ptr<nri::Device, decltype(&nri::nriDestroyDevice)> device(created, &nri::nriDestroyDevice);
        nri::CoreInterface core{};
        nri::WrapperD3D12Interface wrapper{};
        status = nri::nriGetInterface(*device, NRI_INTERFACE(nri::CoreInterface), &core);
        if (status == nri::Result::SUCCESS)
        {
            status = nri::nriGetInterface(*device, NRI_INTERFACE(nri::WrapperD3D12Interface), &wrapper);
        }
        if (status != nri::Result::SUCCESS)
        {
            return cxx::unexpected(nativeError(status));
        }
        const auto& desc = core.GetDeviceDesc(*device);
        EnabledDeviceFeatures enabled;
        enabled.multiview = desc.features.layerBasedMultiview;
        enabled.texture_compression_bc = desc.features.textureCompressionBC;
        enabled.texture_descriptors = desc.tiers.bindless != 0;
        enabled.buffer_descriptors = desc.tiers.bindless != 0;
        enabled.variable_descriptor_count = desc.tiers.bindless != 0;
        enabled.multi_draw_indirect = true;
        // SM 6.6 lacks native SV_StartInstanceLocation; the existing per-draw member binding supplies the offset.
        enabled.indirect_first_instance = false;
        enabled.shader_target = rdesc::EShaderTarget::DXIL;
        // Raw GPU pointers are a separate shader capability; D3D12 uses the contiguous instance implementation.
        if (desc.shaderModel < 606 || !enabled.supports(options))
        {
            return cxx::unexpected(RenderError{kDeviceUnsupported});
        }
        session.device_ = device.release();
        session.nri_core_ = core;
        session.backend_ = new DeviceSession::BackendData{{}, wrapper};
        session.enabled_features_ = enabled;
        session.vertex_stride_limit_ = D3D12_REQ_MULTI_ELEMENT_STRUCTURE_SIZE_IN_BYTES;
        session.vertex_offset_limit_ = D3D12_REQ_MULTI_ELEMENT_STRUCTURE_SIZE_IN_BYTES;
        session.upload_alignment_ = desc.memoryAlignment.uploadBufferTextureSlice;
        for (std::size_t index = 1; index < session.format_support_.size(); ++index)
        {
            session.format_support_[index] = core.GetFormatSupport(session.device(), static_cast<nri::Format>(index));
        }
        for (unsigned role = 0; role < types.size(); ++role)
        {
            nri::Queue* native = nullptr;
            status = core.GetQueue(session.device(), types[role], indices[role], native);
            if (status != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(status));
            }
            const auto found = std::ranges::find_if(
                session.physical_queues_,
                [&](const auto& queue)
                { return core.GetQueueNativeObject(&queue->native()) == core.GetQueueNativeObject(native); }
            );
            const auto index = static_cast<std::uint8_t>(found - session.physical_queues_.begin());
            if (found == session.physical_queues_.end())
            {
                const bool timestamps =
                    types[role] == nri::QueueType::COPY ? desc.features.timestampCopyQueue : desc.features.timestamp;
                const QueueProperties
                    facts{types[role], static_cast<std::uint32_t>(types[role]), {1, 1, 1}, timestamps ? 64U : 0U};
                auto queue = SubmissionQueue::create(session, *native, facts, capacity);
                if (!queue)
                {
                    return cxx::unexpected(queue.error());
                }
                session.physical_queues_.push_back(std::move(*queue));
            }
            session.queue_roles_[role] = index;
        }
        if (options.texture_descriptors)
        {
            // Shared tables are CPU copy sources; each frame owns and qualifies its shader-visible heap pair.
            return session.descriptor_table_.initialize(options, UINT32_MAX);
        }
        return {};
#endif
    }

    RenderResult<void>
    DeviceBackend::validateTextureD3D12(const DeviceSession& session, const nri::TextureDesc& texture) noexcept
    {
#ifndef _WIN32
        return cxx::unexpected(RenderError{kDeviceUnsupported});
#else
        const auto& limits = session.description().dimensions;
        const auto dimension =
            texture.type == nri::TextureType::TEXTURE_1D
                ? limits.texture1DMaxDim
                : (texture.type == nri::TextureType::TEXTURE_3D ? limits.texture3DMaxDim : limits.texture2DMaxDim);
        const auto layers = std::max(texture.layerNum, nri::Dim_t{1});
        const auto samples = std::max(texture.sampleNum, nri::Sample_t{1});
        const bool is_invalid_extent = !texture.width || texture.width > dimension || texture.height > dimension ||
                                       texture.depth > limits.texture3DMaxDim || layers > limits.textureLayerMaxNum;
        const bool is_invalid_mips =
            !texture.mipNum ||
            texture.mipNum > std::bit_width(std::max({texture.width, texture.height, texture.depth}));
        if (is_invalid_extent || is_invalid_mips)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        auto* device = static_cast<ID3D12Device*>(session.nriCore().GetDeviceNativeObject(&session.device()));
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS quality{};
        quality.Format = static_cast<DXGI_FORMAT>(nri::nriConvertNRIFormatToDXGI(texture.format));
        quality.SampleCount = samples;
        const auto result =
            device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &quality, sizeof(quality));
        if (FAILED(result) || quality.NumQualityLevels == 0)
        {
            return cxx::unexpected(RenderError{kDeviceUnsupported});
        }
        return {};
#endif
    }
} // namespace lux::render::detail
