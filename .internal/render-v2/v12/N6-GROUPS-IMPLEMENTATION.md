# N6 instance membership and indirect execution checkpoint

Parent `8ba2ccea7ebf0a1b55cb0556e3bcd452388cf1e0`. This replaces the prior checkpoint's delete-Busy
restriction for selection-associated Instance bindings. Raw matrix-only consumers keep that conservative
contract because they do not promise to follow membership indirection.

InstanceDrawGroup is a complete MeshId/MaterialId/submesh value, not a new runtime ID/Owner. Private Group
is a derived cold draw selection owned by the existing InstanceStorage. Its three actual mapped snapshots
contain one20-byte indexed-indirect command and dense stable SlotMap indices. Multiple Context bindings
share them. The matrix table remains the same three sparse snapshots. InstanceId is still solely assigned
by the existing SlotMap; Entry caches its actual key for cold iteration, not a second identity allocator.

firstInstance is always0 and drawCount1. The vertex program reads group word5+gl_InstanceIndex and then
the selected matrix record. No unenabled multiDrawIndirect/drawIndirectFirstInstance feature is required.
Both buffer views use existing runtime-record Shader reflection, automatic descriptor layout and Graph
read usages. No second Graph, Compiler, Transfer, request table, worker or retirement system.

Create preflights the declared matrix/group capacity and stages group data on a completed non-current
bank. Only after matrix writes also succeed does the owner publish both bindings. Failure leaves current
membership and CPU authority unchanged and erases the unexposed key. Release stages swap-last removal,
publishes it and erases the logical key. A reused index cannot overwrite an in-flight matrix snapshot.
Every physical bank reserves retirement before publication and retains its own real LastUse. Last use
withdrawal retires the derived selection; there is no per-frame live-group scan or permanent cache.

Capacity is the Graph-declared prefix, not a hardcoded business limit. Increasing beyond an attached
prefix requires an explicit cold candidate. Transform updates retain the prior sparse dirty-list path;
structural membership writes copy that group's bounded word table and are not claimed to be sparse.

Tests preserve all prior Instance assertions. Native data test adds two shared Contexts, failed group
flush, failure after membership stage but before matrix publication, accepted-GPU delete/reuse, old/new
actual Copy readbacks, group saturation/Busy and return to pre-group physical byte count. All three Queue
configurations run. New real indirect Draw test uses uploaded vertices/indices, two Camera matrices,
depth test/write, complete reflected matrix/word layouts and numeric pixels. One compiled graph draws
two instances, removes/reuses a slot, rejects declared-capacity overflow, updates Transform, then continues
through the remaining Context after one detach. It is a targeted Native test, not the final Cooked Material
Feature/Runtime3D sample. That distinction remains explicit.

Initial CMake test setup referenced a nonexistent geometry asset target; the logged configure failed and
was corrected to the actual generated Geometry.frag.spv dependency. No placeholder target was added.
Implementation full all -j4 -- -k0, second no-work, serial CTest:140/140139.82s.
Independent results and full source/SDK/protection evidence are recorded separately after exact-I V.
N6.4 combined typed installation/BasicMesh/DepthOnly and N6.5 Host/Client sample remain unfinished;
N7 costs/wait/fairness and final qualification also remain. Performance/IPC PARTIAL, product BLOCKED
and untested-platform qualifications are unchanged.
