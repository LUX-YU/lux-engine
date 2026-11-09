# Scene

SceneInstance owns an externally constructed World, Simulation and its SceneSystems. Systems are destroyed before
World and the Registry they borrow. Engine owns the adopted Scene through SceneSession and builds its scheduling graphs.
SceneInstance does not own the process Context, select concrete Systems, create windows, or submit Render frames.

`description` contains authoring data; `system` declares SceneSystem; `composition` implements SceneInstance ownership.
Descriptions do not replace the current C++ external-assembly path. The remaining old dynamic assembly interfaces have
real consumers and are not an alternative supported SceneRuntime lifecycle. Their unresolved migration is tracked externally.

Retirement requests stop admission, settles accepted tasks, then releases resources. A failed release retains its owner.
ECS observers only record intent; Engine dispatches asynchronous adoption at its Main safe points.
