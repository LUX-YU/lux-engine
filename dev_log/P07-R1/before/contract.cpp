// Full SDK target contract, NOT compiled in the review container.
// Before the correction these assertions should fail specifically on type traits.
// After SDK reinstallation they must pass. Link to the existing preview/compilation targets.
#include <lux/engine/editor/material/MaterialCompilation.hpp>
#include <lux/engine/editor/flowforge/FlowCompilationService.hpp>
#include <memory>
#include <type_traits>
using M = lux::editor::material::MaterialCompileOperation;
using F = lux::editor::flowforge::FlowCompileOperation;
static_assert(!std::is_copy_constructible_v<M>);
static_assert(!std::is_copy_assignable_v<M>);
static_assert(!std::is_move_constructible_v<M>);
static_assert(!std::is_move_assignable_v<M>);
static_assert(!std::is_copy_constructible_v<F>);
static_assert(!std::is_copy_assignable_v<F>);
static_assert(!std::is_move_constructible_v<F>);
static_assert(!std::is_move_assignable_v<F>);
static_assert(std::is_move_constructible_v<std::unique_ptr<M>>);
static_assert(std::is_move_assignable_v<std::unique_ptr<M>>);
int main() {}
