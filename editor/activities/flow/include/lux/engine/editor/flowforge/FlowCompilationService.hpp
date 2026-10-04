#pragma once
#include <lux/engine/editor/flowforge/FlowSnapshot.hpp>
#include <lux/engine/editor/flowforge/FlowEnvironment.hpp>
#include <lux/engine/flowforge/Compiler.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>
namespace lux::services
{
    struct ServiceDescriptor;
}
namespace lux::editor::flowforge
{
    // Registration is cold. The factory borrows lux.process.execution from the root scope;
    // the requesting scope retains one actual compiler/result owner until that scope is closed.
    extern const services::ServiceDescriptor kFlowCompilationService;

    enum class EFlowCompilationError : std::uint8_t
    {
        BUSY,
        CAPACITY,
        CANCELLED,
        INVALID_ID,
        WRONG_THREAD,
        NO_LINKER
    };
    using VFlowCompilationFailure = std::variant<
        EFlowCompilationError,
        lux::flowforge::FlowSourceFailure,
        lux::flowforge::FlowForgeFailure,
        asset::AssetDecodeFailure,
        asset::AssetEncodeFailure,
        process::EExecutionError>;
    template <class T> using FlowCompilationResult = lux::cxx::expected<T, VFlowCompilationFailure>;
    struct FlowCompileId final
    {
        std::uint64_t value{};
        friend bool operator==(FlowCompileId, FlowCompileId) = default;
    };
    struct FlowCompileSettings final
    {
        std::uint64_t version{1};
        std::size_t byte_limit{64U * 1024U * 1024U};
    };
    struct LinkSettings final
    {
        std::filesystem::path executable;
        std::uint64_t version{1};
    };
    struct FlowCompileKey final
    {
        sessions::ContentStamp content;
        std::uint64_t configuration{1}, environment{1};
        friend bool operator==(FlowCompileKey, FlowCompileKey) = default;
    };
    namespace detail { struct FlowCompilation; }
    class CompiledFlow final
    {
    public:
        CompiledFlow(const CompiledFlow&) = default;
        CompiledFlow(CompiledFlow&&) = default;
        CompiledFlow& operator=(const CompiledFlow&) = delete;
        CompiledFlow& operator=(CompiledFlow&&) = delete;
        [[nodiscard]] const FlowCompileKey& key() const noexcept { return key_; }
        [[nodiscard]] const std::shared_ptr<const lux::flowforge::FlowSource>& source() const noexcept
        {
            return source_;
        }
        [[nodiscard]] const std::shared_ptr<const lux::script::ScriptArtifactAsset>& artifact() const noexcept
        {
            return artifact_;
        }
        [[nodiscard]] const lux::cxx::SharedBytes<>& bytes() const noexcept { return bytes_; }

    private:
        friend struct detail::FlowCompilation;
        CompiledFlow(FlowCompileKey key, std::shared_ptr<const lux::flowforge::FlowSource> source,
            std::shared_ptr<const lux::script::ScriptArtifactAsset> artifact, lux::cxx::SharedBytes<> bytes)
            : key_(key), source_(std::move(source)), artifact_(std::move(artifact)), bytes_(std::move(bytes)) {}
        const FlowCompileKey key_;
        const std::shared_ptr<const lux::flowforge::FlowSource> source_;
        const std::shared_ptr<const lux::script::ScriptArtifactAsset> artifact_;
        const lux::cxx::SharedBytes<> bytes_;
    };
    struct FlowLinkAttempt final
    {
        std::uint64_t number{};
        LinkSettings settings;
        process::TaskId task;
        std::optional<VFlowCompilationFailure> failure;
        bool complete{};
    };
    class FlowCompilationService;
    class FlowCompileOperation final
    {
    public:
        ~FlowCompileOperation();
        // The service owns task control; callers retain an ID or borrow a const reference.
        FlowCompileOperation(const FlowCompileOperation&) = delete;
        FlowCompileOperation& operator=(const FlowCompileOperation&) = delete;
        FlowCompileOperation(FlowCompileOperation&&) = delete;
        FlowCompileOperation& operator=(FlowCompileOperation&&) = delete;
        [[nodiscard]] FlowCompileId id() const noexcept;
        [[nodiscard]] FlowCompileKey key() const noexcept;
        [[nodiscard]] sessions::ObservationVersion observed() const noexcept;
        [[nodiscard]] process::TaskId task() const noexcept;
        [[nodiscard]] bool ready() const noexcept;
        [[nodiscard]] bool retryable() const noexcept;
        [[nodiscard]] std::span<const FlowLinkAttempt> attempts() const noexcept;
        [[nodiscard]] std::shared_ptr<const lux::flowforge::FlowForgeObject> object() const noexcept;
        [[nodiscard]] FlowCompilationResult<std::shared_ptr<const CompiledFlow>> result() const;
        void cancel() noexcept;

    private:
        friend class FlowCompilationService;
        struct Impl;
        explicit FlowCompileOperation(std::shared_ptr<Impl>);
        std::shared_ptr<Impl> impl_;
    };
    class FlowCompilationService final
    {
    public:
        explicit FlowCompilationService(process::ExecutionRuntime&, std::size_t capacity = 16);
        ~FlowCompilationService();
        FlowCompilationService(const FlowCompilationService&) = delete;
        FlowCompilationService& operator=(const FlowCompilationService&) = delete;
        FlowCompilationService(FlowCompilationService&&) = delete;
        FlowCompilationService& operator=(FlowCompilationService&&) = delete;
        [[nodiscard]] FlowCompilationResult<FlowCompileId> start(
            FlowSnapshot,
            FlowEnvironment = FlowEnvironment{},
            FlowCompileSettings = {},
            LinkSettings = {}
        );
        [[nodiscard]] FlowCompilationResult<void> retryLink(FlowCompileId, LinkSettings);
        // The borrowed operation remains valid until acknowledge(id) or service destruction.
        [[nodiscard]] FlowCompilationResult<std::reference_wrapper<const FlowCompileOperation>> operation(FlowCompileId
        ) const;
        [[nodiscard]] FlowCompilationResult<void> acknowledge(FlowCompileId);
        [[nodiscard]] FlowCompilationResult<void> cancel(FlowCompileId);
        // Results survive observers. Acknowledgement is explicit and cannot discard an undelivered completion.
        // The full SessionId includes its generation; a replacement session never inherits this result.
        [[nodiscard]] FlowCompilationResult<std::optional<FlowCompileId>> latest(sessions::SessionId) const noexcept;
        [[nodiscard]] FlowCompilationResult<std::vector<FlowCompileId>> snapshotIds() const;
        [[nodiscard]] bool empty() const noexcept;
        // Completion fact only, not an instruction to acknowledge results. Called on the owner thread.
        [[nodiscard]] bool settled() const noexcept;


    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
