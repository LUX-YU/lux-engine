# Lux Engine Process Execution — Canonical Reference Package

Status: **Design / implementation handoff candidate**  
Repository baseline reviewed: `25992bb578ccc7765a0f238c582f62d5424352d1`  
Verified production SHA used by the latest L1 closure evidence: `07453d479538a0cae4037ea18725390c715b4bda`

This package is intentionally **not a port of `legacy/engine/runtime/execution`**. It is a clean
Process design derived from the frozen Lux architecture and the C++26 sender/receiver model.

## One-sentence architecture

> Process is a thin execution substrate: standard Sender/Receiver/Scheduler/Scope semantics are
> used directly; Process only supplies engine-specific asynchronous primitives and adapters.

## What is deliberately absent

There is no:

- `ProcessRuntime`
- `AsyncRuntime`
- `AsyncRuntimeBuilder`
- operation registry
- dependency catalog
- `AsyncOperationContext`
- manual `Completion&&`
- Process-owned parallel CPU pool
- Process-owned main-thread loop
- Process-owned Render thread
- asset/model/material/texture workflow
- coroutine runtime
- compatibility layer for the legacy API

Application/domain code defines its own senders and receivers. Example:

```cpp
auto load_texture(VfsPath path)
{
    return files_.read(std::move(path))
        | stdexec::continues_on(cpu_)
        | stdexec::then([](FileBytes bytes) noexcept {
              return decode_texture(std::move(bytes));
          })
        | stdexec::let_value([this](DecodedTexture texture) {
              return renderer_.upload_texture(std::move(texture));
          })
        | stdexec::continues_on(main_);
}
```

`renderer_.upload_texture()` is itself a Sender. The caller does not switch to a Render scheduler
first and does not see Render channels, callback registries, request ids or Vulkan objects.

## Proposed production topology

```text
engine/
  process/
    CMakeLists.txt                 # collection only

    execution/                     # leaf
      CMakeLists.txt
      include/lux/engine/process/
        Timer.hpp
        PortSender.hpp
      src/
        Timer.cpp
      test/

    io/                            # leaf
      CMakeLists.txt
      include/lux/engine/process/io/
        File.hpp
      src/
        File.cpp
      test/
```

`execution` owns generic Process execution primitives.  
`io` owns byte/file I/O only. It knows nothing about assets.

## Included reference implementation

The code under `proposed/` provides production-shaped implementations for:

- bounded, cancellable `TimerClient::after()` sender;
- bounded `FileClient::read()` / `readRange()` sender;
- `OperationPort<T>` → typed sender adapter;
- no per-request callback registry;
- operation state itself is the lifetime anchor;
- no C++ exceptions as semantic error propagation;
- blocking-file backend is a portable reference backend with bounded preallocated ingress;
- native IOCP/io_uring replacement is explicitly backend-only and requires no public API change.

The portable file backend is intentionally not claimed to be the final fastest platform backend.
The API is designed so native asynchronous file backends can replace it without changing users.

## Read in this order

1. `docs/ARCHITECTURE.zh-CN.md`
2. `docs/PUBLIC_API.zh-CN.md`
3. `docs/MAIN_LOOP_AND_RENDER.zh-CN.md`
4. `docs/COROUTINE_RESERVATION.zh-CN.md`
5. `docs/IMPLEMENTATION_PLAN.zh-CN.md`
6. `docs/PERFORMANCE_GATES.zh-CN.md`
7. `docs/DYNAMIC_FANOUT_DECISION.zh-CN.md`
8. `docs/LEGACY_MIGRATION_MATRIX.zh-CN.md`
9. `VALIDATION_NOTES.md`

## Important implementation note

The reference code targets the current NVIDIA `stdexec` vocabulary (`stdexec::sender_tag`,
`stdexec::receiver_tag`, `stdexec::get_parallel_scheduler`, etc.), which tracks C++26
`std::execution`. Once the project toolchain provides the corresponding standard library
implementation, migration should replace the library namespace/include dependency rather than
introducing a Lux wrapper DSL.

The package was generated as an architecture handoff artifact; it was not compiled inside this
ChatGPT runtime because the Lux repository and its stdexec dependency are not mounted here.
The included tests and integration gates are the required validation entry point when applied to
the repository.
