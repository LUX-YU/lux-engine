# engine/execution unit tests (opt-in: -DENABLE_EXECUTION_TEST=ON).
#
# render_request_sender_test — Phase B. Self-checking exe (exit 0 on success, no
# GoogleTest). Compiling the RenderRequestSender adapter (connect / when_all /
# sync_wait instantiation) IS the stdexec-conformance check; the body drives a
# RenderRequest to completion deterministically (no GPU / server / threads).

find_package(stdexec CONFIG REQUIRED)

add_executable(render_request_sender_test render_request_sender_test.cpp)

target_link_libraries(render_request_sender_test
    PRIVATE
    execution                       # the adapter header (+ render/asset via PUBLIC link)
    STDEXEC::stdexec                 # the adapter header is an opt-in stdexec includer
    lux::engine::function::render    # RenderRequestFactory / FrameReplies / ReplyRecord (explicit)
)

# scheduler_exposure_test — Phase C1. Self-checking exe. Validates that
# EngineExecutorSenders.hpp (cpuScheduler / mainScheduler / spawn over the void*
# handles) exposes the executor's pool + main-thread join point; the COMPILE is the
# conformance check, the RUN asserts the cpu-step-off-main / main-step-on-pump contract.
add_executable(scheduler_exposure_test scheduler_exposure_test.cpp)

target_link_libraries(scheduler_exposure_test
    PRIVATE
    execution                       # EngineExecutorSenders.hpp + asset via PUBLIC link
    STDEXEC::stdexec                 # opt-in stdexec includer
)

# async_cache_test — Phase C2. AsyncCache<K,V> (over a real EngineExecutor) +
# whenAllReady dynamic-arity counting join (drives RenderRequest factories manually).
add_executable(async_cache_test async_cache_test.cpp)

target_link_libraries(async_cache_test
    PRIVATE
    execution                       # AsyncCache + RenderRequestJoin headers (+ asset via PUBLIC)
    STDEXEC::stdexec                 # opt-in stdexec includer
    lux::engine::function::render    # RenderRequestFactory / FrameReplies / ReplyRecord
)

# stdexec's heavy macro metaprogramming + two-phase lookup need these MSVC
# conformance switches at the includer. `execution` keeps them PRIVATE (its public
# header is stdexec-free), so these opt-in stdexec consumers carry their own.
if(MSVC)
    target_compile_options(render_request_sender_test
        PRIVATE /permissive- /Zc:__cplusplus /Zc:preprocessor)
    target_compile_options(scheduler_exposure_test
        PRIVATE /permissive- /Zc:__cplusplus /Zc:preprocessor)
    target_compile_options(async_cache_test
        PRIVATE /permissive- /Zc:__cplusplus /Zc:preprocessor)
endif()
