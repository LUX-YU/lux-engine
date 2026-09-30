// Declaration-only probe. The two class declarations below are copied from the
// reviewed public headers at f6598e65f9d77620f1374065a76d82e6e6e00d79.
// Other types are minimal declarations. This does NOT compile the Lux SDK or
// execute its task runtime. Destructors are never ODR-used by these type traits.
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
#include <iostream>
namespace lux::process { class ExecutionRuntime; struct TaskId; }
namespace lux::material { struct MaterialSource; }
namespace lux::flowforge { struct FlowForgeObject; }
namespace lux::editor::transition {
    class MaterialCompilationAccess;
}
namespace lux::editor::sessions { struct ObservationVersion; }
namespace lux::editor::material {
    namespace process = lux::process;
    class MaterialSnapshot;
    struct MaterialCompileSettings {};
    struct MaterialCompileKey;
    struct MaterialCompileId;
    struct CompiledMaterial;
    template <class T> class MaterialCompileResult;
// BEGIN verbatim class declaration: MaterialCompilation.hpp
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
// END verbatim class declaration
}
namespace lux::editor::flowforge {
    namespace process = lux::process;
    struct FlowCompileId;
    struct FlowCompileKey;
    struct FlowLinkAttempt;
    struct CompiledFlow;
    template <class T> class FlowCompilationResult;
    class FlowCompilationService;
// BEGIN verbatim class declaration: FlowCompilationService.hpp
    class FlowCompileOperation final
    {
    public:
        ~FlowCompileOperation();
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
// END verbatim class declaration
}

template<class T> void show(const char* name) {
    std::cout << name
        << " copy_constructible=" << std::is_copy_constructible_v<T>
        << " copy_assignable=" << std::is_copy_assignable_v<T>
        << " move_constructible=" << std::is_move_constructible_v<T>
        << " move_assignable=" << std::is_move_assignable_v<T>
        << '\n';
}
int main() {
    using M = lux::editor::material::MaterialCompileOperation;
    using F = lux::editor::flowforge::FlowCompileOperation;
    show<M>("MaterialCompileOperation");
    show<F>("FlowCompileOperation");
#ifdef REQUIRE_UNIQUE_OWNER
    static_assert(!std::is_copy_constructible_v<M>, "Material operation owner must not copy");
    static_assert(!std::is_copy_assignable_v<M>, "Material operation owner must not copy-assign");
    static_assert(!std::is_move_constructible_v<M>, "Move unique_ptr, not operation object");
    static_assert(!std::is_move_assignable_v<M>, "Move unique_ptr, not operation object");
    static_assert(!std::is_copy_constructible_v<F>, "Flow service operation must not copy");
    static_assert(!std::is_copy_assignable_v<F>, "Flow service operation must not copy-assign");
    static_assert(!std::is_move_constructible_v<F>, "Flow service retains stable object");
    static_assert(!std::is_move_assignable_v<F>, "Flow service retains stable object");
#endif
}
