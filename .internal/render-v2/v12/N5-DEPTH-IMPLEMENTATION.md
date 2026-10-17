# N5 Depth / attachment formats — implementation checkpoint

Parent db30b88e064e84975542d72ed4a61197a855c5fe. This checkpoint follows actual Vertex/Index/Indirect and
Compute implementation. N5 combined independent qualification is pending; N6 has not started.

Graph owns a DEPTH_ATTACHMENT role; PassProgram owns explicit clear depth and GraphicsState owns
DepthTest/Write/Compare. Existing immutable CompiledGraph owns Pipeline identity including actual color
and Depth formats/state. Existing ExecutionContext owns the physical images/views and prepared recipes;
no additional Graph, executor, queue, resource owner or retirement mechanism. Target remains final color.
Private ImageFormat only maps supported formats, byte size and aspects; actual device capabilities remain
checked. D16/D32 use DEPTH_STENCIL_ATTACHMENT_OPTIMAL without requiring separateDepthStencilLayouts.

Image creation/view, barrier aspect, render attachment, explicit clear and Image→Buffer copy use actual
formats. Pipeline accepts a Vertex-only depth Pass or Vertex+Fragment color Pass. Duplicate/wrong stages,
invalid compare/clear values and missing required Depth attachment reject cold. Color-only Contexts still
have no implicit readback. All shared attachments in a rendering scope share Target resize constraints;
the cold extent propagation does not reorder or re-analyze the Graph. Only a copy of the final color
selects default readback; other exported Buffers use their explicit Graph resource index. Depth rendering
scopes conservatively remain separate; original legal color-only R5 merge/count behavior is unchanged.

Real GPU test: 3 color formats (RGBA8,RGBA16F,RGBA32F) × 2 Depth formats (D16,D32) × 6 scenarios:
LESS occlusion, disabled first Depth write, GREATER comparison, Vertex-only Depth pass, two color
attachments sharing Depth with reverse resize propagation, explicit transfer Depth clear. Every case
prepares a32x24 Context from16x16 definition, executes3 frames, checks all color/depth values and byte
counts, and observes first-party new=0 in record/execute/wait window. Five Shader/state negatives per
case plus color-only missing-Depth rejection. No GPU timing or performance-equivalence claim.

Implementation Windows Release all -j4 -- -k0 / no-work; initial targeted33/33,28.39s and full114/114,
126.27s. Subsequent resize/boundary changes: all/no-work and4/4,1.73s; final extra explicit Depth clear:
all/no-work and1/1,0.53s. Exact final combined independent R/D/ASan must still run. Validation/SyncValidation
Error0 in executed tests; EOS duplicate-layer warning preserved. First build failed because the test wrote
bytes into the parameter mapping field; corrected to parameter_values, original failed log retained.
No tests executed against the failed build. No production optimization is inferred from these results.

Evidence E:/RenderV2Rebuild/V12/evidence/n55-depth contains raw commands, exits, logs, asset/compiler hashes
and protection. Legacy719,main/Layer user modifications,history/dependencies protected. SDK sync will use
exact I. N5 combined V will also independently cover preceding Geometry I. Root historical BLOCKED,
IPC/performance PARTIAL, upper Engine/Editor and Linux/Android/TSan NOT_RUN remain unchanged.
