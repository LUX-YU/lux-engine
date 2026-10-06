# Skeleton authoring SDK example

This is an external DLL consumer, not code linked into the editor. Build against a complete installed SDK:

```text
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PREFIX_PATH=<sdk> -DSDK_PREFIX=<sdk> -DEC1_MODE=HEADLESS
cmake --build build --target all -j 4 -- -k 0
ctest --test-dir build --output-on-failure
```

Use `WINDOW` to qualify detached construction and actual Root ownership without a renderer. `APP` explicitly
links the installed application and its LLVM/render dependencies; it runs the real offscreen desktop. Pass the
usual dependency toolchain/LLVM/MLIR paths when required by that SDK. CPU modes reject importing the concrete
tool UI or application targets. They do not construct a render runtime or perform compilation.

The same `skeleton_extension` is used in all modes. Its declared activation capability is SessionActivities.
`Model.cpp` owns only the Skeleton working copy, domain operations and original-codec encoding. The SDK owns
SessionStore, SessionState admission/checkpoint, the single EditHistory/EditExecutor algorithm, SaveService,
WriteCoordinator and Process execution. No host Skeleton dispatch branch is installed.

`Extension.cpp` contributes a source factory, typed command and complete detached Pane factory. Each Pane
borrows a session key, owns its controls and retains a draft with its captured content stamp. Apply cannot
silently rebase; stale input remains visible until Revert. There is no node reordering or reparent operation:
bone order, mesh indices, bind and inverse-bind matrices remain unchanged. The editable properties are the
root name and global X translation. Author snapshots use the existing SkeletonAsset codec.

The application qualification additionally installs `witness_extension`. This test-only observer explicitly
requests project and workbench capabilities to inspect the asset browser and answer review dialogs. It
contributes no content or editing algorithm and cannot broaden the Skeleton plugin's capabilities. Probe.hpp
and the witness are instrumentation, not an alternate application API. All observed providers remain borrowed.

The assertions exercise source VFS reads on the blocking scheduler, owner-thread installation, history,
Save/SaveAs, reload preparation under the read gate, two views sharing one session, stale identities, callback
close rejection, exact source routing and recovery. HEADLESS/WINDOW release the probe DLL handle before work:
actual factory/session/view leases keep callbacks valid until the final owner is destroyed. APP removes the
Skeleton provider, preserves its catalog/files and proves a built-in Material can still be opened.

The command-line executable enables assertions in optimized builds. Initial fixture creation uses ordinary
file writes; saves under test use SaveExecution and the real FileArtifactStore. Observations are not a claim
of manual input, IME, Linux qualification, or arbitrary hot unloading support.
