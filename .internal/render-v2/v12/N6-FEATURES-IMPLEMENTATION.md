# N6 production Mesh/Depth Feature checkpoint

Parent 13d12b6a490b73a8c3c7c09e0b6e4cd22fefd0e8. BasicMeshFeature and DepthOnlyFeature are actual
neutral descriptions/build callbacks with a typed MeshFeatureConfig. The Native provider generates
PassProgram and typed Storage arguments; Provider::Storages uses the preceding atomic installation
transaction. No runtime Feature callback, new Graph, resource Owner, upload path or Shader parser.

Global Mesh/Material resources and Scene Instance matrices/indirect group records are explicitly placed.
Both Features execute real indexed-indirect geometry using the existing group snapshot, Transfer,
Descriptor and SubmissionRequirements machinery. BasicMesh uses the previously cooked Unlit material
fragment and actual Camera view/projection matrices. DepthOnly emits a vertex-only depth draw and a
separate color Target clear. Its independent D32 map has the configured fixed resolution; changing that
resolution requires Effect reconfiguration. Resizing only the color Target does not silently change it.
BasicMesh color/depth attachments share the target extent. Explicit optional readback declares both
image-to-buffer copies; ordinary Feature assembly has no hidden host buffer or copy.

The first Feature's vertex contract is position RGB32 at location0 and (BasicMesh) UV RG32 at location1,
RGBA8 UNORM/SRGB color and D32 depth. This is a specific Unlit Feature, not a restriction of the general
Native compiler. Existing wider format/Compute/depth/indexed tests remain. Dynamic parameters update
existing physical Material bindings without compiling a new Feature graph/program.

render_native_vulkan_extensions no longer publicly links Feature assembly. The real static
render_native_vulkan_features component owns the Native providers and generated vertex SPIR-V.
Public neutral and Native headers are independently compiled. The relocated consumer first proves
base Native and Storage extensions do not import Feature assembly, then explicitly imports the new
provider component. The older Texture consumer now explicitly links the Feature library it really uses.

New neutral and actual GPU tests cover cold contracts/budget/errors, two shared tetrahedron instances,
color and D32 numeric output, three Queue configurations, material update, detach/delete isolation,
and target resize with the distinct depth contracts above. All 143 Release tests pass in 251.25s,
full all -j4 -- -k0 and second no-work; prior141 and original102 names retained. This is implementation
evidence; independent exact-I ASan/installation qualification follows. N6.5 Host/Client example and N7
remain open, including runtime selection of additional explicit readback outputs.

Preserved failed attempts: missing direct ExecutionContext test include; Native resource names initially
omitted G5's shared: prefix (fixed only in the new provider); installed Texture fixture missing its
explicit Feature link. Three incrementally linked test images failed; CDB on Feature registration shows the pre-main CRT
__scrt_initialize_type_info jumping through a null InitializeSListHead import. Original EXE/PDB/ILK and
stacks are archived. Removing those generated EXE/ILK files and relinking, without Runtime/IPC/CRT source
changes, makes all three tests pass. The final full run passes too; independent fresh linking remains
mandatory. Temporary numeric line diagnostics in Compile/LayoutPlan were restored to their original tracked contents.

Legacy719, three user files, histories and protected IPC algorithms remain intact. No performance
improvement claim: IPC/historical unexplained performance stays PARTIAL. Root product closure stays
BLOCKED; upper Engine/Editor, Linux/Android and TSan NOT_RUN. No GPU timestamp measurement.
