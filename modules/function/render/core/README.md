# Render Core

Core contains neutral Render values: typed object identities, target and viewport descriptions,
structured errors, C++ type qualifications, immutable configuration values and execution counters.
It owns no renderer objects, threads, GPU resources or runtime directory.

Scene/View/Target/Effect/Attachment/Feature/Storage identities use distinct `cxx::SlotKey<Tag>` domains.
Their actual owners allocate and validate generations. A null check does not prove membership;
equal numeric keys in different owners are unrelated. GraphPassId and GraphResourceId are local
indices into one immutable Graph Definition, not process-wide resource identities.

`TargetDesc` describes final color output. Validation checks dimensions, format and sample shape;
device support is checked by Native. `GraphOutputRequirements` belongs to Graph and expresses
compatibility with a Target. Neither object is a GPU lifetime or completion token.

`RenderResult<T>` uses `cxx::expected<T, RenderError>` for fallible internal functions. Command
Responses instead carry an explicit `RenderError error` and their business fields. Hosts register
static error descriptions and choose diagnostic output; Core performs no terminal I/O.

`ConfigValue` exclusively owns one immutable C++ configuration and is move-only. Its type-specific
destructor and semantic comparison remain with the defining module. Assembly borrows it synchronously;
an asynchronous compile request owns its inputs until work ends. Dynamic configurations count their
actual owned capacity through `ConfigValueTraits`; externally borrowed upload bytes are not owned
configuration memory. Hosts keep the defining module loaded through final value destruction.
There is no Render-specific configuration wire codec, shared owner or second type registry.

Upload byte lifetime is a separate contract: after successful submission the Client preserves the
source address, extent and content until Response. Failure and cancellation also end CPU reading.
Abandon or disconnection provides no such proof; the Host settles Server and Producers before
releasing an input without a Response. Native independently owns staging and submitted GPU content.

Cooked `SpirvStage` and `SpirvArtifact` live in `lux::rdesc` under resource/description. They own code,
entry names and reflected ABI facts, without Vulkan handles or a compiler dependency. They are not
duplicated in Core. Persistent encoding remains in the existing resource codecs.

## Build and installation

Use `find_package(lux-engine-render CONFIG REQUIRED COMPONENTS render_core)` and link
`lux::engine::render::render_core`. It depends on lux-cxx value/container utilities and the canonical
error/resource value targets. It does not find Vulkan, VMA or stdexec. The synchronous Client source
configuration uses this same closure. There is no `render_data` compatibility component or old-path
forwarding header.

Build `all -j 4 -- -k 0` from the Render bootstrap or an external SDK consumer. The installed headers
come from their owning modules; no stale SDK copy is used to satisfy a missing source dependency.
Whole-product qualification and historical results are separate from this module's API contract.
