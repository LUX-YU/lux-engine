#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace lux::rdesc
{
    enum class EPassFieldRole
    {
        SAMPLED_READ,
        STORAGE_READ,
        STORAGE_WRITE,
        STORAGE_READ_WRITE,
        SAMPLER,
        UNIFORM_READ,
        READ_ONLY_STORAGE,
        READ_WRITE_STORAGE,
        COLOR_ATTACHMENT,
        DEPTH_STENCIL,
        RESOLVE,
        TRANSFER_SOURCE,
        TRANSFER_DESTINATION,
        VERTEX,
        INDEX,
        INDIRECT,
        INPUT_ATTACHMENT
    };

    [[nodiscard]] constexpr bool isShaderDescriptorRole(EPassFieldRole role) noexcept
    {
        switch (role)
        {
        case EPassFieldRole::SAMPLED_READ:
        case EPassFieldRole::STORAGE_READ:
        case EPassFieldRole::STORAGE_WRITE:
        case EPassFieldRole::STORAGE_READ_WRITE:
        case EPassFieldRole::SAMPLER:
        case EPassFieldRole::UNIFORM_READ:
        case EPassFieldRole::READ_ONLY_STORAGE:
        case EPassFieldRole::READ_WRITE_STORAGE:
        case EPassFieldRole::INPUT_ATTACHMENT:
            return true;
        default:
            return false;
        }
    }

    [[nodiscard]] constexpr bool isPassFieldRole(EPassFieldRole role) noexcept
    {
        if (isShaderDescriptorRole(role))
        {
            return true;
        }
        switch (role)
        {
        case EPassFieldRole::COLOR_ATTACHMENT:
        case EPassFieldRole::DEPTH_STENCIL:
        case EPassFieldRole::RESOLVE:
        case EPassFieldRole::TRANSFER_SOURCE:
        case EPassFieldRole::TRANSFER_DESTINATION:
        case EPassFieldRole::VERTEX:
        case EPassFieldRole::INDEX:
        case EPassFieldRole::INDIRECT:
            return true;
        default:
            return false;
        }
    }

    enum class EFieldOwner
    {
        SCENE,
        FEATURE,
        PASS_LOCAL
    };
    enum class EUpdateFrequency
    {
        STATIC,
        FRAME,
        DRAW
    };
    enum class EScalarKind
    {
        FLOAT,
        INT,
        UINT
    };

    // Generated cold metadata. Names are diagnostics/compile facts, never runtime routes.
    struct PassResourceField
    {
        std::string_view path;
        std::string_view shader_name;
        EPassFieldRole role;
        EFieldOwner owner;
        EUpdateFrequency frequency;
        bool required;
        std::uint32_t array_count;
        std::uint32_t element_stride;
        std::string_view semantic;
        std::string_view paired_texture;
        std::string_view dimension;
        std::string_view image_format;
        std::uint32_t stages{7};
        bool descriptor_array{false};
        std::uint32_t element_alignment{1};

        bool operator==(const PassResourceField&) const noexcept = default;
    };

    struct PassScalarField
    {
        std::string_view path;
        EScalarKind kind;
        std::uint32_t offset;
        std::uint32_t size;
        std::uint32_t array_stride;
        std::uint32_t array_count;
        EFieldOwner owner{EFieldOwner::PASS_LOCAL};
        EUpdateFrequency frequency{EUpdateFrequency::FRAME};
        std::uint32_t stages{7};

        bool operator==(const PassScalarField&) const noexcept = default;
    };

    struct PassShaderContract
    {
        std::string_view canonical_name;
        std::span<const PassResourceField> resources;
        std::span<const PassScalarField> scalars;
        std::string_view declarations;
        std::uint32_t parameter_size;
        std::uint32_t parameter_alignment;
        std::array<std::string_view, 3> stage_declarations{};
    };
} // namespace lux::rdesc
