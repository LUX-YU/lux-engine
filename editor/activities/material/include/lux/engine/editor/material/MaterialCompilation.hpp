#pragma once
#include <lux/engine/editor/material/MaterialSnapshot.hpp>
#include <lux/engine/material/Compiler.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>
#include <lux/engine/resource/asset/material/MaterialAssets.hpp>
#include <optional>
namespace lux::editor::material
{
    enum class EMaterialCompileRequestError : std::uint8_t
    {
        BUSY,
        CAPACITY,
        CANCELLED,
        INVALID_ID,
        WRONG_THREAD
    };
    using VMaterialCompileFailure = std::variant<
        EMaterialCompileRequestError,
        lux::material::MaterialCompileFailure,
        asset::AssetDecodeFailure,
        asset::AssetEncodeFailure,
        process::EExecutionError>;
    template <class T> using MaterialCompileResult = lux::cxx::expected<T, VMaterialCompileFailure>;
    struct MaterialCompileId final
    {
        std::uint64_t value{};
        friend bool operator==(MaterialCompileId, MaterialCompileId) = default;
    };
    struct MaterialCompileSettings final
    {
        std::uint64_t version{1};
        std::size_t byte_limit{64U * 1024U * 1024U};
        friend bool operator==(MaterialCompileSettings, MaterialCompileSettings) = default;
    };
    struct MaterialCompileInputKey final
    {
        sessions::ContentStamp content;
        MaterialCompileSettings settings;
        std::uint64_t environment{1};
        friend bool operator==(const MaterialCompileInputKey&, const MaterialCompileInputKey&) = default;
    };
    namespace detail { struct MaterialCompilation; }
    class CompiledMaterial final
    {
    public:
        CompiledMaterial(const CompiledMaterial&) = default;
        CompiledMaterial(CompiledMaterial&&) = default;
        CompiledMaterial& operator=(const CompiledMaterial&) = delete;
        CompiledMaterial& operator=(CompiledMaterial&&) = delete;
        [[nodiscard]] const MaterialCompileInputKey& key() const noexcept { return key_; }
        [[nodiscard]] const std::shared_ptr<const lux::material::MaterialSource>& source() const noexcept
        {
            return source_;
        }
        [[nodiscard]] const std::shared_ptr<const asset::MaterialAsset>& artifact() const noexcept { return artifact_; }
        [[nodiscard]] const lux::cxx::SharedBytes<>& bytes() const noexcept { return bytes_; }

    private:
        friend struct detail::MaterialCompilation;
        CompiledMaterial(MaterialCompileInputKey key, std::shared_ptr<const lux::material::MaterialSource> source,
            std::shared_ptr<const asset::MaterialAsset> artifact, lux::cxx::SharedBytes<> bytes)
            : key_(key), source_(std::move(source)), artifact_(std::move(artifact)), bytes_(std::move(bytes)) {}
        const MaterialCompileInputKey key_;
        const std::shared_ptr<const lux::material::MaterialSource> source_;
        const std::shared_ptr<const asset::MaterialAsset> artifact_;
        const lux::cxx::SharedBytes<> bytes_;
    };
    class MaterialCompileOperation final
    {
    public:
        [[nodiscard]] static MaterialCompileResult<std::unique_ptr<MaterialCompileOperation>> start(
            process::ExecutionRuntime&,
            MaterialSnapshot,
            MaterialCompileSettings = {},
            std::uint64_t environment = 1
        );
        ~MaterialCompileOperation();
        // Transfer the unique_ptr returned by start(), never the task control object itself.
        MaterialCompileOperation(const MaterialCompileOperation&) = delete;
        MaterialCompileOperation& operator=(const MaterialCompileOperation&) = delete;
        MaterialCompileOperation(MaterialCompileOperation&&) = delete;
        MaterialCompileOperation& operator=(MaterialCompileOperation&&) = delete;
        [[nodiscard]] MaterialCompileId id() const noexcept;
        [[nodiscard]] process::TaskId task() const noexcept;
        [[nodiscard]] MaterialCompileInputKey key() const noexcept;
        [[nodiscard]] sessions::ObservationVersion observed() const noexcept;
        [[nodiscard]] bool ready() const noexcept;
        void cancel() noexcept;
        [[nodiscard]] MaterialCompileResult<std::shared_ptr<const CompiledMaterial>> result() const;

    private:
        struct Impl;
        explicit MaterialCompileOperation(std::shared_ptr<Impl>);
        std::shared_ptr<Impl> impl_;
    };
}
