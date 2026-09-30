#include <lux/engine/editor/material/MaterialCompilation.hpp>
#include <lux/engine/editor/flowforge/FlowCompilationService.hpp>
#include <iostream>
#include <type_traits>
template<class T> void traits(const char* name) {
 std::cout << name << " copy_ctor=" << std::is_copy_constructible_v<T> << " copy_assign=" << std::is_copy_assignable_v<T>
 << " move_ctor=" << std::is_move_constructible_v<T> << " move_assign=" << std::is_move_assignable_v<T> << '\n'; }
int main() { traits<lux::editor::material::MaterialCompileOperation>("MaterialCompileOperation");
traits<lux::editor::flowforge::FlowCompileOperation>("FlowCompileOperation"); }
