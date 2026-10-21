#pragma once

#include <array>
#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/resource/StorageDescriptor.hpp>
#include <variant>
#include <vector>

namespace lux::render
{
    enum class EDirectLightingBuffer : std::uint8_t
    {
        DIRECTIONAL,
        POINT,
        SPOT,
        COUNTS
    };

    // Scene-local world space; linear radiance. Directions are unit vectors pointing from the light.
    struct DirectionalLightGpu final
    {
        std::array<float, 4> radiance{1, 1, 1, 0};
        std::array<float, 4> direction{0, 0, -1, 0};
    };

    struct PointLightGpu final
    {
        std::array<float, 4> position_range{0, 0, 1, 10};
        std::array<float, 4> radiance{1, 1, 1, 0};
        std::array<float, 4> attenuation{1, 0, 1, 0};
    };

    struct SpotLightGpu final
    {
        std::array<float, 4> position_range{0, 0, 1, 10};
        std::array<float, 4> radiance{1, 1, 1, 0};
        std::array<float, 4> direction_outer{0, 0, -1, 0.7F};
        std::array<float, 4> attenuation_inner{1, 0, 1, 0.9F};
    };

    static_assert(sizeof(DirectionalLightGpu) == 32 && sizeof(PointLightGpu) == 48 && sizeof(SpotLightGpu) == 64);
    using VDirectLight = std::variant<DirectionalLightGpu, PointLightGpu, SpotLightGpu>;
    struct DirectLightTag;
    using LightId = StorageResourceId<DirectLightTag>;
    inline constexpr StorageNameView kDirectLightingStorageType{"lux.render.storage.DirectLighting"};

    struct DirectLightingBinding final
    {
        EDirectLightingBuffer field;
        bool operator==(const DirectLightingBinding&) const noexcept = default;
    };

    struct DirectLightUpdate final
    {
        LightId light;
        VDirectLight value;
    };

    // The Client retains input addresses and contents through Response. Inputs are deduplicated and finite;
    // abandon/close does not end the borrow: the application must settle Server/CPU producers first.
    struct CreateDirectLights final
    {
        static constexpr std::string_view name = "lux.render.CreateDirectLights";

        struct Request final
        {
            StorageId storage;
            std::uint64_t sequence = 0;
            std::span<const VDirectLight> values;
        };

        struct Response final
        {
            RenderError error{};
            std::uint64_t sequence = 0;
            std::vector<LightId> lights;
        };
    };

    struct UpdateDirectLights final
    {
        static constexpr std::string_view name = "lux.render.UpdateDirectLights";

        struct Request final
        {
            StorageId storage;
            std::uint64_t sequence = 0;
            std::span<const DirectLightUpdate> values;
        };

        struct Response final
        {
            RenderError error{};
            std::uint64_t sequence = 0;
            std::uint32_t changed = 0;
        };
    };

    struct RemoveDirectLight final
    {
        static constexpr std::string_view name = "lux.render.RemoveDirectLight";

        struct Request final
        {
            LightId light;
            std::uint64_t sequence = 0;
        };

        struct Response final
        {
            RenderError error{};
            std::uint64_t sequence = 0;
        };
    };

    struct DirectLightingStorageDescriptor final
    {
        using Config = void;
        using Ops = void;
        static inline const StorageDescriptor descriptor{
            kDirectLightingStorageType,
            0,
            0,
            cppType<DirectLightingBinding>(),
            "Scene direct-light GPU arrays: directional, finite point and spot lights, with independent FIF replicas."
        };
    };
} // namespace lux::render
