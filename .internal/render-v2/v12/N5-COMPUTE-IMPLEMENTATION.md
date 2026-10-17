# N5.5 basic Compute — implementation checkpoint

Parent `412233224b1d1ca4bf8c1ab8037c307ce08078c8` (lglsl asset implementation). N5 IN_PROGRESS.
Graph retains the sole version/dependency Analysis; STORAGE_WRITE and STORAGE_READ_WRITE are orthogonal
execution roles checked by the existing Builder. Full ShaderBinding identity now includes read/write access;
SPIRV-Cross block decorations prove it. No new Shader parser or Graph representation. One existing LayoutPlan
resolves transient/imported Buffer descriptors and actual Vertex/Fragment/Compute stage visibility.

CompiledGraph owns Compute pipelines beside its existing Graphics pipelines. Dispatch is a compiled value
recipe; ExecutionContext owns prepared descriptors/parameters and transient backing, all submitted through
the original queue/Ticket/LastUse/retirement mechanism. No new resource Owner, queue, Handler or worker.
The existing Graphics queue must really expose Compute. Workgroup counts and fixed local sizes are validated
against device limits before creation. Specialized LocalSizeId is explicitly unsupported, not silently read
as a literal. Imported persistent write semantics are not added. This is basic Compute, not async scheduling
or GPU Culling. Initial transient Buffer barriers order reusable-frame reads/writes.

Actual GPU sequence: write0.125 -> read/write x2 -> Graphics and Buffer Copy. Ten repeated executions produce
RGBA64 and exact float0.25 with first-party execute/wait new=0. Invalid author roles, dispatch0/device overflow,
access mismatch, missing entry, wrong actual write qualifier and nonexistent stage metadata reject. Existing
last-good/Texture/Buffer/retirement/Code Pin tests remain. Original halfway UNORM oracle failed (Buffer exactly
0.5, device RGBA127 rather than asserted128); changed input to avoid the quantization tie, retaining exact
checks instead of adding tolerance. Both failed logs preserved; original intermediate SPIR-V was not archived.

Windows Release full all -j4 -- -k0, second no-work, full112/112124.38s. Final formatting-only change rebuilt
all, second no-work and targetedCompute1/1,0.46s. Targeted Graph/Native14/14,6.08s preceded full run. GPU
Validation/SyncValidation errors0; actual EOS warning retained. Independent exact-I ASan follows; no final
N5 PASS. Generated source/compiler/binaries hashes and raw attempts: E:/RenderV2Rebuild/V12/evidence/n55-compute.

N5 still requires Vertex/Index/Depth/Indirect, formats and final combined gate; N6/N7 NOT_STARTED. Historical
performance PARTIAL and Root TOOLCHAIN BLOCKED unchanged. Legacy719/history/user changes protected.
