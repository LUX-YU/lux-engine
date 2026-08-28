# Design references

## Repository baseline

- Main observed for this package: `25992bb578ccc7765a0f238c582f62d5424352d1`
- Latest production fix: `07453d479538a0cae4037ea18725390c715b4bda`
- Latest evidence records the worker-event data-race closure and exact-SHA DEVELOPER / PLAYER /
  TOOLCHAIN validation plus benchmark v12.

## C++ execution model

The design intentionally follows the C++26 sender/receiver model rather than inventing a Lux DSL.

Reference material reviewed in August 2026:

- C++26 `std::execution` / scheduler overview:
  https://en.cppreference.com/cpp/execution
- Scheduler concept:
  https://en.cppreference.com/cpp/execution/scheduler
- P2300R10:
  https://www9.open-std.org/JTC1/SC22/WG21/docs/papers/2024/p2300r10.html
- stdexec current user/developer/reference docs:
  https://nvidia.github.io/stdexec/
  https://nvidia.github.io/stdexec/user/
  https://nvidia.github.io/stdexec/developer/
  https://nvidia.github.io/stdexec/reference/

Key design facts used:

- Sender is lazy and composable.
- connect(sender, receiver) produces an immovable operation state.
- operation state must live until completion.
- scheduler is a lightweight handle to an execution resource.
- value/error/stopped are distinct completion channels.
- async scope is the structured owner for dynamically spawned operations.
- the system/parallel scheduler is intended to represent a shared process-wide parallel execution resource.
