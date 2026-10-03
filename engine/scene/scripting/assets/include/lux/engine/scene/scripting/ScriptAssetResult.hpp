#pragma once

#include <lux/cxx/container/ScopeId.hpp>
#include <lux/cxx/container/SlotMap.hpp>
#include <lux/engine/core/semantic/SemanticType.hpp>
#include <lux/engine/resource/asset/AssetTypeId.hpp>
#include <lux/engine/resource/identity/AssetId.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>

namespace lux::scene::script
{
    struct ScriptAssetScopeTag;
    struct ScriptAssetResultTag;

    struct ScriptAssetHandle final
    {
        lux::cxx::ScopeId<ScriptAssetScopeTag> domain;
        lux::cxx::SlotKey<ScriptAssetResultTag> slot;
        [[nodiscard]] bool valid() const noexcept { return domain.isValid() && slot.isValid(); }
        friend bool operator==(ScriptAssetHandle, ScriptAssetHandle) noexcept = default;
    };

    enum class EScriptAssetError : std::uint32_t
    {
        INVALID_INPUT = 1,
        CAPACITY_EXCEEDED,
        STOPPING,
        INVALID_HANDLE,
        NOT_READY,
        TYPE_MISMATCH,
        INVALID_RANGE,
        WRONG_THREAD,
        EXECUTION_REJECTED
    };

    enum class EScriptAssetFailureDomain : std::uint32_t
    {
        NONE,
        STORAGE,
        DECODE,
        SUBMIT,
        EXECUTION,
        CANCELLED
    };

    class ScriptAssetReadOutcome final
    {
    public:
        [[nodiscard]] static ScriptAssetReadOutcome success(ScriptAssetHandle handle) noexcept
        {
            return {handle, EScriptAssetFailureDomain::NONE, 0};
        }
        [[nodiscard]] static ScriptAssetReadOutcome
        failure(EScriptAssetFailureDomain domain, std::uint32_t code) noexcept
        {
            return {{}, domain, code};
        }
        [[nodiscard]] bool succeeded() const noexcept { return handle_.valid(); }
        [[nodiscard]] ScriptAssetHandle handle() const noexcept { return handle_; }
        [[nodiscard]] EScriptAssetFailureDomain errorDomain() const noexcept { return domain_; }
        [[nodiscard]] std::uint32_t errorCode() const noexcept { return code_; }

    private:
        ScriptAssetReadOutcome(ScriptAssetHandle handle, EScriptAssetFailureDomain domain, std::uint32_t code) noexcept
            : handle_(handle), domain_(domain), code_(code)
        {
            if ((domain_ == EScriptAssetFailureDomain::NONE) != handle_.valid())
                std::terminate();
        }
        ScriptAssetHandle handle_;
        EScriptAssetFailureDomain domain_;
        std::uint32_t code_{};
    };

    struct ScriptAssetDescription final
    {
        lux::asset::AssetId id;
        lux::asset::AssetTypeId type;
        std::uint64_t image_bytes{};
        bool decoded{};
        bool has_image{};
    };

    struct AssetByteChunk final
    {
        static constexpr std::uint32_t Capacity = 256;
        std::array<std::byte, Capacity> bytes{};
        std::uint32_t size{};
    };
}

namespace lux::semantic
{
    template <> struct TTypeTraits<lux::scene::script::ScriptAssetReadOutcome> final
    {
        inline static constexpr std::string_view CanonicalName = "lux.scene.script.AssetReadOutcome.v1";
        inline static constexpr std::uint8_t AbiKind = static_cast<std::uint8_t>(EAbiKind::STRUCT_REF);
        inline static constexpr std::uint32_t Size = sizeof(lux::scene::script::ScriptAssetReadOutcome);
        inline static constexpr std::uint32_t Alignment = alignof(lux::scene::script::ScriptAssetReadOutcome);
    };
}
