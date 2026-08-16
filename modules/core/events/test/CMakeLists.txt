# Events module tests. Enable with -DENABLE_EVENTS_TEST=ON.
#
# domain_events_test: full behavioral coverage of the unified event bus —
#   delivery/FIFO/multi-pump/batch sharing, publish-during-drain round
#   semantics, three backpressure policies + dropped counts + diagnostics,
#   Subscription RAII/move/self-unsubscribe, emergencyDrain, a 4-producer
#   400k-event lock-free stress run, and InlineRecord pack/unpack.

add_executable(
    domain_events_test
    domain_events_test.cpp
)

target_link_libraries(
    domain_events_test
    PRIVATE
    events
)

# CPU-only, self-contained; the "cpu" label folds it into the suite that
# `ctest -L cpu` already runs.
add_test(NAME domain_events_test COMMAND domain_events_test)
set_tests_properties(domain_events_test PROPERTIES LABELS "cpu" TIMEOUT 120)
