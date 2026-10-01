#include <lux/engine/editor/project/ProjectCatalogModel.hpp>
#include <lux/engine/editor/tasks/TaskMonitor.hpp>
#include <lux/engine/editor/material/MaterialCompilationService.hpp>
#include <lux/engine/editor/flowforge/FlowCompilationService.hpp>
#include <type_traits>
#include <cstdio>
template<class T> constexpr bool fixedOwner = !std::is_copy_constructible_v<T> && !std::is_copy_assignable_v<T>
    && !std::is_move_constructible_v<T> && !std::is_move_assignable_v<T>;
static_assert(fixedOwner<lux::editor::project::ProjectCatalogModel>);
static_assert(fixedOwner<lux::editor::tasks::TaskMonitor>);
static_assert(fixedOwner<lux::editor::material::MaterialCompilationService>);
static_assert(fixedOwner<lux::editor::flowforge::FlowCompilationService>);
int main() { std::puts("Four actual SDK owners reject all four copy/move special members"); }
