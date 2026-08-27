import lux.simulation


class EnemyBehavior:
    @lux.method
    def tick(
        self,
        step: lux.simulation.SimulationStepInfo,
        collision: "lux.test.CollisionEvent",
        delta: "lux.f32",
    ) -> "lux.i32":
        return 1

    def helper(self, value: "lux.i32") -> None:
        pass
