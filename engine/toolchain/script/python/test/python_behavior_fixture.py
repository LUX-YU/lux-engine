from lux.simulation import SimulationStepInfo as StepInfo


class EnemyBehavior:
    @lux.method
    def tick(self, step: StepInfo, delta: "lux.f32") -> "lux.i32":
        return 1

    def helper(self, value: "lux.i32") -> None:
        pass
