#pragma once
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/function/visibility.h>
#include <optional>
#include <system_error>
#include <string_view>

namespace lux::render
{
    /**
     * @brief Unified render error codes for all render subsystems.
     *
     * Replaces the per-resource enums (EMeshResError, EMaterialError,
     * ELightError, ETextureResError) with a single taxonomy registered
     * into std::error_category so that every API returns
     * Expected<T> = expected<T, std::error_code>.
     */
    enum class ERenderError
    {
        Success = 0,

        // --- generic ---
        Unknown,
        InvalidArgument,
        OutOfMemory,
        NotFound,
        IOError,
        UnsupportedFormat,
        DeviceLost,
        Timeout,
        PermissionDenied,
        AlreadyExists,
        Busy,
        Canceled,
        DataCorrupted,
        NetworkError,
        NotImplemented,
        InternalError,

        // --- render-target / swapchain ---
        SwapchainCreateFailed,
        DepthImageCreateFailed,
        FramebufferCreateFailed,
        RenderPassCreateFailed,

        // --- resource subsystem (unified from per-resource enums) ---
        AssetNotFound,          ///< Asset ID could not be resolved
        AssetInvalid,           ///< Asset data is null or corrupt
        GpuUploadFailed,        ///< GPU-side texture/buffer upload failed
        SsboFull,               ///< SlicedSSBO capacity exhausted
        UnsupportedType,        ///< Material/light type not supported
        TypeMismatch,           ///< Modify called with wrong material type
        ModifyFailed,           ///< In-place SSBO modify returned false
        MissingTransform,       ///< Light requires a world transform

        // --- render graph ---
        GpuResourceCreationFailed,      ///< VMA image/buffer creation failed
        InvalidCompiledGraph,           ///< Compiled graph is invalid or in bad state
        VulkanObjectCreationFailed,     ///< Vulkan object (semaphore, cmd pool, etc.) creation failed
        RecordContextAllocationFailed,  ///< RGRecordContext allocation failed

        // --- lifecycle / teardown ---
        ShutdownStillPending,           ///< flushShutdownCleanup() called while async work still pends
        ShutdownNotBegun,               ///< flushShutdownCleanup() called before beginShutdown()

        // --- feature-type registration ---
        NullFeatureFactory,             ///< a feature factory has no create_fn (addFeature would crash)
        FeatureTypeCollision,           ///< a DIFFERENT factory claims an already-registered stable feature-type id
    };

    class RenderErrorCategory : public std::error_category
    {
    public:
        const char* name() const noexcept override
        {
            return "RenderErrorCategory";
        }

        std::string message(int ev) const override
        {
            switch (static_cast<ERenderError>(ev))
            {
            case ERenderError::Success:                return "Success";
            case ERenderError::Unknown:                return "Unknown error";
            case ERenderError::InvalidArgument:        return "Invalid argument";
            case ERenderError::OutOfMemory:            return "Out of memory";
            case ERenderError::NotFound:               return "Not found";
            case ERenderError::IOError:                return "I/O error";
            case ERenderError::UnsupportedFormat:      return "Unsupported format";
            case ERenderError::DeviceLost:             return "Device lost";
            case ERenderError::Timeout:                return "Timeout";
            case ERenderError::PermissionDenied:       return "Permission denied";
            case ERenderError::AlreadyExists:          return "Already exists";
            case ERenderError::Busy:                   return "Resource busy";
            case ERenderError::Canceled:               return "Operation canceled";
            case ERenderError::DataCorrupted:          return "Data corrupted";
            case ERenderError::NetworkError:           return "Network error";
            case ERenderError::NotImplemented:         return "Not implemented";
            case ERenderError::InternalError:          return "Internal error";
            case ERenderError::SwapchainCreateFailed:  return "Swapchain creation failed";
            case ERenderError::DepthImageCreateFailed: return "Depth image creation failed";
            case ERenderError::FramebufferCreateFailed:return "Framebuffer creation failed";
            case ERenderError::RenderPassCreateFailed: return "Render pass creation failed";
            case ERenderError::AssetNotFound:          return "Asset not found";
            case ERenderError::AssetInvalid:           return "Asset data invalid";
            case ERenderError::GpuUploadFailed:        return "GPU upload failed";
            case ERenderError::SsboFull:               return "SSBO capacity exhausted";
            case ERenderError::UnsupportedType:        return "Unsupported material/light type";
            case ERenderError::TypeMismatch:           return "Material type mismatch on modify";
            case ERenderError::ModifyFailed:           return "In-place resource modify failed";
            case ERenderError::MissingTransform:       return "Light requires world transform";
            case ERenderError::GpuResourceCreationFailed: return "GPU resource creation failed (VMA)";
            case ERenderError::InvalidCompiledGraph:    return "Compiled render graph is invalid";
            case ERenderError::VulkanObjectCreationFailed: return "Vulkan object creation failed";
            case ERenderError::RecordContextAllocationFailed: return "Record context allocation failed";
            case ERenderError::ShutdownStillPending:   return "Shutdown still has pending async work";
            case ERenderError::ShutdownNotBegun:       return "Shutdown cleanup requested before beginShutdown";
            case ERenderError::NullFeatureFactory:     return "Feature factory has no create function";
            case ERenderError::FeatureTypeCollision:   return "Feature type id collision (two factories share one stable id)";
            default:                                   return "Unknown error code";
            }
        }
    };

    LUX_FUNCTION_PUBLIC const RenderErrorCategory& renderErrorCategory();

    inline std::error_code make_error_code(ERenderError e)
    {
        return std::error_code{static_cast<int>(e), renderErrorCategory()};
    }

    template<typename T> using Expected = lux::cxx::expected<T, std::error_code>;
}
