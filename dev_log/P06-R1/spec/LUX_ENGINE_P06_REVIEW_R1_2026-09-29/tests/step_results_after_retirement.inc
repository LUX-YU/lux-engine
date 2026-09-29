// REVIEW TEST FRAGMENT — NOT BUILT OR RUN IN THE REVIEW ENVIRONMENT.
// Add inside the anonymous namespace of
// editor/tools/scene/execution/test/runs.cpp, using its existing Fixture/take helpers.
// Register the functions in main() and the existing CMake foreach; do not add a business target.
// The assertions express the requested corrected contract, not current passing behavior.

void reviewPrintStep(const char* label,
                     const lux::editor::scene::RunResult<lux::scene::SceneStepStatus>& value)
{
    std::printf("%s available=%d", label, bool(value));
    if (value)
        std::printf(" state=%u result_ok=%d", unsigned(value->state), bool(value->result));
    else if (const auto* runtime =
                 std::get_if<lux::scene::SceneRuntimeFailure>(&value.error().cause))
    {
        if (const auto* error = std::get_if<lux::scene::ESceneRuntimeError>(&runtime->cause))
            std::printf(" runtime_error=%u invalid_instance=%d", unsigned(*error),
                        *error == lux::scene::ESceneRuntimeError::INVALID_ID);
    }
    std::puts("");
    std::fflush(stdout);
}

// R06-R1-01 + 02: no Run acknowledgement has occurred. The heavy instance has gone,
// but one completed and two cancelled, unacknowledged step outcomes remain observable.
void reviewStepResultsAfterStop()
{
    using namespace lux::editor::scene;
    using S = lux::scene::ESceneStepState;
    Fixture f;
    const auto run = f.start();
    assert(f.runs.pause(run));
    f.until([&] { return take(f.runs.info(run)).state == ERunState::PAUSED; });
    const auto author_before = f.author->describe();
    const auto instance = take(f.runs.info(run)).instance;

    const auto done = take(f.runs.step(run));
    f.until([&] { return take(f.runs.stepStatus(done)).state == S::COMPLETED; });
    // Deliberately do not acknowledge 'done'.
    const auto queued1 = take(f.runs.step(run));
    const auto queued2 = take(f.runs.step(run));
    const auto stopped = take(f.runs.stop(run));
    assert(!stopped.complete());
    f.until([&] { return stopped.complete(); });
    assert(f.runs.update());
    assert(take(f.runs.info(run)).state == ERunState::STOPPED);
    assert(!f.runtime->borrowInstance(instance));
    assert(!f.runtime->borrowClock(instance));

    // Query for the FIRST TIME after reclamation, before acknowledgeStop(run).
    const auto a = f.runs.stepStatus(done);
    const auto b = f.runs.stepStatus(queued1);
    const auto c = f.runs.stepStatus(queued2);
    reviewPrintStep("completed_after_retirement", a);
    reviewPrintStep("cancelled_1_after_retirement", b);
    reviewPrintStep("cancelled_2_after_retirement", c);
    std::printf("run_still_present=%d stop_complete=%d\n", bool(f.runs.info(run)), stopped.complete());
    std::fflush(stdout);

    assert(a && a->state == S::COMPLETED && a->result);
    assert(b && b->state == S::CANCELLED && !b->result);
    assert(c && c->state == S::CANCELLED && !c->result);
    assert(f.runs.acknowledgeStep(done));
    assert(f.runs.acknowledgeStep(queued1));
    assert(f.runs.acknowledgeStep(queued2));
    assert(!f.runs.stepStatus(done));
    assert(!f.runs.acknowledgeStep(done));

    const auto author_after = f.author->describe();
    assert(author_after.current == author_before.current);
    assert(author_after.observed == author_before.observed);
    assert(author_after.binding == author_before.binding);
    assert(author_after.dirty == author_before.dirty);
    assert(f.runs.acknowledgeStop(run));
    assert(!f.runs.info(run) && !f.runs.stepStatus(queued1));
    std::puts("R06-R1-01/02: reclaimed instance, retained exact outcomes, explicit acknowledgement PASS");
}

// R06-R1-03: do not depend on a consumer polling in the narrow interval between
// drive failure and actual instance reclamation. Keep the original failure value.
void reviewFailedStepsAfterRetirement()
{
    using namespace lux::editor::scene;
    using S = lux::scene::ESceneStepState;
    Fixture f;
    const auto run = f.start();
    assert(f.runs.pause(run));
    f.until([&] { return take(f.runs.info(run)).state == ERunState::PAUSED; });
    const auto instance = take(f.runs.info(run)).instance;
    {
        auto registry = take(f.runtime->borrowInstance(instance));
        registry.get().ctx().get<Probe::Facts>().fail = true;
    }
    const auto first = take(f.runs.step(run));
    const auto second = take(f.runs.step(run));
    // The original test inspects a FAILED step before update()/retirement.
    // This test deliberately waits until the run is FAILED and its instance is gone.
    f.until([&] { return take(f.runs.info(run)).state == ERunState::FAILED; });
    assert(!f.runtime->borrowInstance(instance));
    const auto a = f.runs.stepStatus(first);
    const auto b = f.runs.stepStatus(second);
    reviewPrintStep("first_failed_after_retirement", a);
    reviewPrintStep("second_failed_after_retirement", b);
    assert(a && a->state == S::FAILED && !a->result);
    assert(b && b->state == S::FAILED && !b->result);
    for (const auto* value : {&*a, &*b})
    {
        const auto* drive = std::get_if<lux::scene::SceneDriveFailure>(&value->result.error().cause);
        assert(drive && drive->phase == lux::scene::ESceneDrivePhase::PUBLICATION);
        // Also assert the existing Probe's failure payload 731 using its actual typed cause API.
        // Do not rewrite failure as STOPPED/INVALID_ID just to satisfy the outer state check.
    }
    assert(f.runs.acknowledgeStep(first) && f.runs.acknowledgeStep(second));
    assert(f.runs.acknowledgeStop(run));
    std::puts("R06-R1-03: failure outcomes retained after instance retirement PASS");
}
