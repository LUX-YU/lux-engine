# N6.3 Camera production checkpoint

Parent `0cbf920a990aaa42840dea77807e674779ce41d9`. This closes the Camera value/update portion,
not N6 instanced geometry, Feature composition or integrated3D qualification.

View uniquely owns complete column-major view/projection matrices and a normalized viewport/depth
range. CreateView and UpdateCamera validate finite values and ranges at the request boundary.
ExecutionContext borrows copied CPU parameter bytes and stores the viewport; owner-thread publication
updates future recordings without waiting for GPU or altering already accepted commands. Native
setViewport has an explicit validated-caller precondition, avoiding repeated request validation.
Resize preserves camera state and scales the normalized viewport against the new Target extent.

Runtime requires the complete128-byte Camera parameter contract for live programs that consume
context parameters. Generic Native standalone parameter blocks remain supported. ShaderBlock describes
both mat4 fields explicitly; Core Camera is independent of Vulkan. No Command Wire encoding, second
View owner, per-frame name lookup, graph analysis or Pipeline compilation is introduced.

Production View.color is removed. Historical color oracles now live only in Runtime test fixtures:
a real generated fragment Shader reads both Camera matrices, preserving all old pixel assertions.
Native standalone16-byte parameter fixtures continue testing their existing generic Native contract.
The new Camera test covers isolated Views, both matrices, viewport clear/draw pixels, resize, invalid
finite/range values, stale View generation and failed candidate retention. Its wrong-ABI fixture reaches
the actual Runtime cold check, verified by a program callback count, rather than failing in describe.

The initial new frame-observation predicate required exactly128, while the GPU returned127 for UNORM
0.5. Raw failed runs preserve that observation. The predicate now uses the same +/-1 conversion tolerance
as the existing full pixel oracle. CPU Ack is not a displayed-frame guarantee; the bounded observation
loop waits for actual updated pixels. Full pixel/alpha/background assertions remain after observation.

Final implementation full all build, second no-work and serial CTest: **139/139,140.36s**.

Public Core/Data Camera headers compile independently and participate in the existing relocated SDK
consumer matrix. Final exact-I checkpoint results belong in the independent V report. Earlier139-test
implementation runs are evidence only; no old report is amended. Source protection uses the existing
new-stage checker:719 Legacy blobs and three protected user files. Raw commands/failures/generator
inputs and hashes are in external evidence/N6-camera; exact-I SDK backup/sync and independent ASan
follow this commit. Final N7 still requires Release/Debug/ASan and overall product/performance separation.

Performance and historical IPC qualification remain PARTIAL; root TOOLCHAIN historical BLOCKED,
upper Engine/Editor/Linux/Android/TSan NOT_RUN. No3D scene or GPU culling completion is claimed here.
