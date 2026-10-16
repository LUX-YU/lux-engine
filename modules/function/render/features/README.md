# Feature contracts and cold graph assembly

FeatureDescriptor contains stable type identity, exact version and optional encoded configuration Schema. It contains no callback, Command table, Shader or Native object. FeatureGraphOps supplies the synchronous describe/build algorithms; FeatureInstanceView borrows descriptor, graph operations, configuration owners and instance name for one assembly call. The registering module must remain loaded through every callback.

assembleFeatures validates identities and configurations, sorts full type/instance names, resolves claims, declares each shared resource once, invokes builders, finishes once, audits authorship and analyzes once. Command metadata is validated by the unique Runtime registration table, never by graph assembly. Results own one Definition, one Analysis, external GraphStorageBindings and contribution ranges; no input pointer escapes.

Only the provider may write a shared output. Initialized imported and previous-history ports use v0; graph-produced shared ports use v1. Private scratch uses ordinary G4 versions. The contribution's input_index/first_pass/pass_count is the authority for Program authorship. Zero-pass Features need no Program callback. Private pass/resource keys use featureKey's unambiguous type/instance/local encoding.

FeatureRegistration combines metadata, graph callbacks, typed Commands and an optional Program callback. ProgramContribution separates vector<PassProgram> from vector<StorageArgument>. Runtime sends only Definition/Analysis/GraphStorageBindings/Programs/Target to Native, and retains arguments in Effect. Duplicate equal arguments are accepted; unequal values for one resource key fail cold. No mutable arguments enter CompiledGraph identity.

Scenarios A/B/C remain CPU authoring fixtures: Deferred/Lighting/Tonemap, static HZB Previous/Culling, and zero-pass Material Storage/Mesh. They do not claim production advanced rendering. Native tests independently cover the actual Offscreen, Texture and external Buffer path. Existing G5 reports remain historical and unchanged.

The component depends on Graph, Data, Protocol and Resource; no Native, Runtime or IPC dependency. Encoding is explicit configuration serialization, not Command transport. Config bytes and metadata are retained by the caller during synchronous assembly or by the owning compile request during worker execution. No Feature callback runs in stable frames.
