#pragma once

#include <concepts>
#include <exec/task.hpp>
#include <filesystem>
#include <lux/cxx/compile_time/type_info.hpp>
#include <lux/engine/core/execution/Task.hpp>
#include <lux/engine/resource/asset/AssetTypeId.hpp>
#include <lux/engine/resource/identity/AssetId.hpp>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace lux::editor
{
    enum class EAssetImportError : std::uint8_t
    {
        NOT_READY,
        BUSY,
        INVALID_INPUT,
        CANCELLED,
        READ,
        COOK,
        ENCODE,
        PUBLICATION,
        STALE_CATALOG,
        INVALID_CATALOG,
        EXECUTION,
        UNKNOWN_IMPORTER
    };

    struct AssetImportFailure final
    {
        EAssetImportError code{};
        std::string detail;
        // Published or publication durability is uncertain; cancellation does not undo that fact.
        bool catalog_published{};
    };

    template <class T> using AssetImportResult = cxx::expected<T, AssetImportFailure>;

    struct AssetProduct final
    {
        asset::AssetId id;
        // Zero means the version-1 directory did not record this dependency's type.
        // A newly cooked product always has an explicit nonzero type.
        asset::AssetTypeId type;
        friend bool operator==(const AssetProduct&, const AssetProduct&) = default;
    };

    struct AssetImportSource final
    {
        asset::AssetId id;
        std::filesystem::path file;
    };

    // Immutable package plus directory facts; decoded professional assets never enter the catalog.
    struct AssetImportProduct final
    {
        asset::AssetTypeId root_type;
        std::vector<AssetProduct> products;
        std::vector<std::byte> package;
    };

    template <class T>
    concept AssetImporter = requires(
        execution::ExecutionRuntime& execution,
        AssetImportSource source,
        typename T::Configuration configuration,
        std::span<const std::byte> bytes,
        execution::TaskReporter reporter
    ) {
        { T::name } -> std::convertible_to<std::string_view>;
        { T::version } -> std::convertible_to<std::uint32_t>;
        { T::encode(configuration) } noexcept -> std::same_as<AssetImportResult<std::vector<std::byte>>>;
        { T::decode(bytes) } noexcept -> std::same_as<AssetImportResult<typename T::Configuration>>;
        {
            T::cook(execution, std::move(source), std::move(configuration), reporter)
        } noexcept -> std::same_as<exec::task<AssetImportResult<AssetImportProduct>>>;
    };

    // The caller retains configuration bytes until this finite operation finishes.
    using AssetImportCook = exec::task<AssetImportResult<AssetImportProduct>> (*)(
        execution::ExecutionRuntime&,
        AssetImportSource,
        std::span<const std::byte>,
        execution::TaskReporter
    ) noexcept;

    // Cold declaration. The project and accepted import each retain callable code.
    // One importer may produce many asset types; names and versions identify its configuration codec.
    struct AssetImportDeclaration final
    {
        std::shared_ptr<const void> code_lifetime;
        std::string name;
        std::uint32_t version{};
        cxx::type_info configuration_type;
        std::vector<std::byte> default_configuration;
        AssetImportCook cook = nullptr;
    };

    template <AssetImporter T>
    [[nodiscard]] AssetImportResult<AssetImportDeclaration> declareAssetImporter(
        std::shared_ptr<const void> code = {}
    ) noexcept
    {
        auto defaults = T::encode(typename T::Configuration{});
        if (!defaults)
        {
            return cxx::unexpected(defaults.error());
        }
        auto decoded = T::decode(*defaults);
        if (!decoded)
        {
            return cxx::unexpected(decoded.error());
        }
        AssetImportDeclaration result;
        result.code_lifetime = std::move(code);
        result.name = T::name;
        result.version = T::version;
        result.configuration_type = cxx::make_basic_type_info<typename T::Configuration>();
        result.default_configuration = std::move(*defaults);
        result.cook =
            +[](execution::ExecutionRuntime& execution,
                AssetImportSource source,
                std::span<const std::byte> bytes,
                execution::TaskReporter reporter) noexcept -> exec::task<AssetImportResult<AssetImportProduct>>
        {
            auto configuration = T::decode(bytes);
            if (!configuration)
            {
                co_return cxx::unexpected(configuration.error());
            }
            co_return co_await T::cook(execution, std::move(source), std::move(*configuration), reporter);
        };
        return result;
    }
} // namespace lux::editor
