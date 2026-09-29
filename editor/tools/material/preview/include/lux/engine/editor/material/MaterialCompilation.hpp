#pragma once
#include <lux/engine/editor/material/MaterialSnapshot.hpp>
#include <lux/engine/material/Compiler.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>
#include <lux/engine/resource/asset/material/MaterialAssets.hpp>
#include <optional>
#include <any>
namespace lux::editor::transition
{
    class MaterialCompilationAccess;
}
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
    struct MaterialPreviewFailure final
    {
        std::string domain;
        std::any cause;
    };
    using VMaterialCompileFailure = std::variant<
        EMaterialCompileRequestError,
        lux::material::MaterialCompileFailure,
        asset::AssetDecodeFailure,
        asset::AssetEncodeFailure,
        process::EExecutionError,
        MaterialPreviewFailure>;
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
    struct MaterialCompileKey final
    {
        sessions::ContentStamp content;
        MaterialCompileSettings settings;
        std::uint64_t environment{1}, target{1};
        friend bool operator==(const MaterialCompileKey&, const MaterialCompileKey&) = default;
    };
    struct CompiledMaterial final
    {
        MaterialCompileKey key;
        std::shared_ptr<const lux::material::MaterialSource> source;
        std::shared_ptr<const asset::MaterialAsset> artifact;
        lux::cxx::SharedBytes<> bytes;
    };
    class MaterialCompileOperation final
    {
    public:
        [[nodiscard]] static MaterialCompileResult<std::unique_ptr<MaterialCompileOperation>> start(
            process::ExecutionRuntime&,
            MaterialSnapshot,
            MaterialCompileSettings = {},
            std::uint64_t environment = 1,
            std::uint64_t target = 1
        );
        ~MaterialCompileOperation();
        [[nodiscard]] MaterialCompileId id() const noexcept;
        [[nodiscard]] process::TaskId task() const noexcept;
        [[nodiscard]] MaterialCompileKey key() const noexcept;
        [[nodiscard]] sessions::ObservationVersion observed() const noexcept;
        [[nodiscard]] bool ready() const noexcept;
        void cancel() noexcept;
        [[nodiscard]] MaterialCompileResult<std::shared_ptr<const CompiledMaterial>> result() const;

    private:
        friend class lux::editor::transition::MaterialCompilationAccess;
        struct Impl;
        explicit MaterialCompileOperation(std::shared_ptr<Impl>);
        [[nodiscard]] static MaterialCompileResult<std::unique_ptr<MaterialCompileOperation>> startSource(
            process::ExecutionRuntime&,
            std::shared_ptr<const lux::material::MaterialSource>,
            MaterialCompileKey,
            sessions::ObservationVersion
        );
        std::shared_ptr<Impl> impl_;
    };
}
