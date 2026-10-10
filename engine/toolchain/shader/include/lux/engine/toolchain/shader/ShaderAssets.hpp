#pragma once

#include <lux/engine/toolchain/shader/PassValidation.hpp>
#include <memory>
#include <vector>

namespace lux::toolchain
{
    struct ShaderSourceInput
    {
        std::string canonical_path, content;
        bool operator==(const ShaderSourceInput&) const noexcept = default;
    };

    struct ShaderDefine
    {
        std::string name, value;
        bool operator==(const ShaderDefine&) const noexcept = default;
    };

    // Asset name is the stable author ShaderKey name; variant name identifies a replacement slot.
    // Exact source bytes include all includes. Build tools supply this manifest from actual inputs.
    struct ShaderBuildInputs
    {
        std::string asset_name, variant_name, compiler_identity, target_environment;
        std::vector<ShaderSourceInput> sources;
        std::vector<ShaderDefine> defines;
        bool operator==(const ShaderBuildInputs&) const noexcept = default;
    };

    struct ShaderStageBinary
    {
        std::uint32_t stage;
        std::vector<std::uint32_t> words;
        bool operator==(const ShaderStageBinary&) const noexcept = default;
    };

    struct ShaderVariantIdentity
    {
        ShaderBuildInputs inputs;
        // Canonical, length-prefixed encoding of generated metadata. Equality only, not another schema.
        std::vector<std::uint8_t> schema_identity;
        std::vector<ShaderStageBinary> stages;
        bool operator==(const ShaderVariantIdentity&) const noexcept = default;
    };

    [[nodiscard]] std::vector<std::uint8_t> shaderSchemaIdentity(const rdesc::PassShaderContract& schema) noexcept;

    class CompiledShaderVariant
    {
    public:
        [[nodiscard]] const ShaderVariantIdentity& identity() const noexcept
        {
            return identity_;
        }

    private:
        friend cxx::expected<CompiledShaderVariant, std::string>
        makeCompiledShaderVariant(ShaderBuildInputs, std::vector<ShaderStageBinary>, const rdesc::PassShaderContract&) noexcept;

        explicit CompiledShaderVariant(ShaderVariantIdentity identity) noexcept : identity_(std::move(identity)) {}

        ShaderVariantIdentity identity_;
    };

    // Cooked SPIR-V is accepted only after complete program and binary validation.
    [[nodiscard]] cxx::expected<CompiledShaderVariant, std::string> makeCompiledShaderVariant(
        ShaderBuildInputs inputs,
        std::vector<ShaderStageBinary> stages,
        const rdesc::PassShaderContract& schema
    ) noexcept;

    // Cold, owner-thread bounded directory. No runtime watcher, native owner or global registry.
    // Borrowed results last until the next mutation or destruction; native candidates copy identity.
    class ShaderVariantCatalog
    {
    public:
        explicit ShaderVariantCatalog(std::size_t capacity) noexcept;
        ShaderVariantCatalog(ShaderVariantCatalog&&) noexcept = default;
        ShaderVariantCatalog& operator=(ShaderVariantCatalog&&) noexcept = default;
        ShaderVariantCatalog(const ShaderVariantCatalog&) = delete;
        ShaderVariantCatalog& operator=(const ShaderVariantCatalog&) = delete;

        // Candidate is already validated. Replaced storage is returned to the caller, never silently destroyed.
        [[nodiscard]] cxx::expected<std::unique_ptr<const CompiledShaderVariant>, std::string> publish(
            CompiledShaderVariant candidate
        ) noexcept;

        [[nodiscard]] const CompiledShaderVariant* find(const ShaderVariantIdentity& identity) const& noexcept;
        const CompiledShaderVariant* find(const ShaderVariantIdentity&) const&& = delete;

        [[nodiscard]] const CompiledShaderVariant* current(std::string_view asset, std::string_view variant)
            const& noexcept;
        const CompiledShaderVariant* current(std::string_view, std::string_view) const&& = delete;

        [[nodiscard]] cxx::expected<std::unique_ptr<const CompiledShaderVariant>, std::string> remove(
            std::string_view asset,
            std::string_view variant
        ) noexcept;

        [[nodiscard]] std::size_t size() const noexcept
        {
            return entries_.size();
        }

    private:
        std::size_t capacity_;
        std::vector<std::unique_ptr<const CompiledShaderVariant>> entries_;
    };
} // namespace lux::toolchain
