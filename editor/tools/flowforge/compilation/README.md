# Flow compilation

`flowforge_compilation` is a static, independently installed concrete compiler service. Frozen FlowSource and
owned metadata environment accompany CPU compilation. External linking runs on the existing blocking scheduler;
asset encoding returns to CPU. No worker borrows FlowSession, ReadView, Pane or the old Editor.

FlowCompileId identifies retained work; TaskId identifies one executor attempt. retryLink keeps the exact compiled
object and original source stamp, even after author changes. Each record retains at most eight attempts and a
configured byte limit; record capacity includes unacknowledged terminal results. Acknowledge only terminal work.
Completion storage is a leaf owner fact, never a new business admission or recursive confirmation.

FlowCompileEnvironment copies catalog arrays and retains their code owner. Nested descriptor/string pointers
remain covered by the environment's explicit owner contract (or static builtin lifetime). All service calls are
on its owner thread. ExecutionRuntime must survive cancellation/terminal delivery.

publishFlowArtifact shares the existing WriteCoordinator and SaveExecution. It publishes fixed bytes
against an explicit expected target version and never marks author source clean. Legacy UI conversion bridges
are private, consumer-limited, and expire at P12.
