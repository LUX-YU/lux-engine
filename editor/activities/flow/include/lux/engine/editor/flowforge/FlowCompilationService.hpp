#pragma once
#include <lux/engine/editor/flowforge/FlowSnapshot.hpp>
#include <lux/engine/flowforge/Compiler.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>
namespace lux::editor::flowforge
{
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
    // Owns the catalog arrays and keeps their descriptor/module owner alive. Program-lifetime
    // descriptors need no code lease; dynamic descriptors follow FlowSourceEnvironment's contract.
    class FlowCompileEnvironment final
    {
    public:
        explicit FlowCompileEnvironment(lux::flowforge::FlowSourceEnvironment = {}, std::uint64_t version = 1);
        [[nodiscard]] lux::flowforge::FlowSourceEnvironment view() const noexcept;
        [[nodiscard]] std::uint64_t version() const noexcept;

    private:
        struct Data;
        std::shared_ptr<const Data> data_;
    };
    struct FlowCompileKey final
    {
        sessions::ContentStamp content;
        std::uint64_t configuration{1}, environment{1};
        friend bool operator==(FlowCompileKey, FlowCompileKey) = default;
    };
    struct CompiledFlow final
    {
        FlowCompileKey key;
        std::shared_ptr<const lux::flowforge::FlowSource> source;
        std::shared_ptr<const lux::script::ScriptArtifactAsset> artifact;
        lux::cxx::SharedBytes<> bytes;
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
            FlowCompileEnvironment = FlowCompileEnvironment{},
            FlowCompileSettings = {},
            LinkSettings = {}
        );
        [[nodiscard]] FlowCompilationResult<void> retryLink(FlowCompileId, LinkSettings);
        // The borrowed operation remains valid until acknowledge(id) or service destruction.
        [[nodiscard]] FlowCompilationResult<std::reference_wrapper<const FlowCompileOperation>> operation(FlowCompileId
        ) const;
        [[nodiscard]] FlowCompilationResult<void> acknowledge(FlowCompileId);
        [[nodiscard]] FlowCompilationResult<void> cancel(FlowCompileId);
        // Release the caller's interest without cancelling accepted work. Idempotent after acknowledgement.
        [[nodiscard]] FlowCompilationResult<void> releaseResult(FlowCompileId) noexcept;
        // Collects only the released, ready set observed on entry; cleanup-created work waits for another turn.
        [[nodiscard]] FlowCompilationResult<void> collectReleased();
        [[nodiscard]] FlowCompilationResult<std::vector<FlowCompileId>> snapshotIds() const;
        [[nodiscard]] bool empty() const noexcept;


    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
