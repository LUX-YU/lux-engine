# Simulation

Simulation owns TickGroups and their SimulationSystems. A group selects a fixed step or variable advancement;
the system itself declares component access and execution affinity. Group/system assembly precedes Engine admission.

SceneSession, not Simulation, owns the TaskGraphs and actual tick execution times. Engine schedules fixed deadlines
and variable rounds, runs each graph, then applies Main-thread ECS work at the shared safe points.

Concrete Script and Physics2D implementations live under engine/systems. This module defines the common simulation
contract and does not depend on those implementations, rendering, windows, or Editor.
Runtime insertion/removal of individual Systems remains outside the current contract.
