# F6.b type responsibilities

No new public class, native resource owner, thread, manager or registry is introduced.

| Value / algorithm | State, ownership and real consumer | Lifetime / failure / phase |
|---|---|---|
| `PassDataContract` (new struct) | TypeId, canonical name, schema version, Scope and generated GPU layout; neutral projection into existing PassShaderContract, not another schema authority | Borrows generated static arrays during cold Shader validation; explicit ABI rejection |
| `OwnerAssignment` (replaces three members) | field path and ShaderOwnerId; one explicit association per generated descriptor field | Synchronous cold span, copied into existing LayoutIdentity as owned DescriptorLocation; duplicate/missing/unknown paths rejected |
| `OwnerField::data_identity` | Owned canonical DataSchema ABI bytes, including every leaf and version | Cold, no DataSet/native identity; full comparison after hash |
| `DescriptorLocation::field_path` | Owned exact generated resource path | Existing fixed recipe uses it during cold assembly; avoids category/semantic ambiguity |
| `shaderOwnerId` (free algorithm) | Existing FNV primitive over canonical name, revision, category, sorted complete fields and Data ABI | Cold allocation permitted; complete shape remains cache equality authority |
| `DataGpuLayout::operator==` | Full existing value comparison, including fields | No new state or owner |

BindingSlot is represented by existing OwnerShape identity plus full shape, not an extra wrapper.
Owner category remains Scope classification only. `lighting.main` and `lighting.preview` are structural
semantic slots, independent of Scene A/B's actual Buffers. DataSetId/generation/record storage are not added.
Generated metadata borrows static storage; NativeProgram/Layout own their cold identity strings.
Native buffers, descriptors, program owners, Invocation slots and queue tickets retain existing RAII.
No new thread-sharing rule: cold assembly precedes owner-thread fixed execution.

`fixtureOwners` is a test-author helper for unchanged old one-owner-per-category fixtures. It is not
installed, linked into production or used by the new multiple-owner fixture. There is no production
three-slot compatibility adapter.
