# Project IO adapter

`ProjectArtifactStore` publishes one file from owned encoded bytes. Existing ProjectStorage journal
code and this adapter share `editor_file_publication`'s file read/write/flush/replace primitives.
The kernel is a STATIC target in the existing storage directory, with only lux-cxx dependencies;
project_io never links the old ProjectStorage/Editor/editing implementation.

Targets are root-contained canonical filesystem addresses, including relative `.`/`..` aliases and
existing symbolic-link resolution. Windows keys fold case using the invariant locale. Known hard-link
aliases are rejected. Changing a symlink concurrently, case-sensitive Windows directories, filesystem
mount changes, and concurrent external writes between the final check and rename are outside the
single-coordinator guarantee. This is optimistic external-change detection, not global CAS.

The writer creates its own staging directory alongside the target, writes and flushes its payload,
rechecks the expected digest, then replaces the target. A pre-existing staging name is a failure,
never permission to truncate/delete someone else's file. Cleanup removes only this operation's
payload/directory. It is not a journal for project-wide multi-file transactions.

Windows uses FlushFileBuffers and MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH); POSIX uses fsync and
rename. A successful replacement is Published, separately from power-loss durability. Failure in the
optional post-replace durability callback produces a Published receipt with an UNCONFIRMED warning.
No directory-fsync or global crash-atomic guarantee is claimed. Reconciliation verifies bytes after
this synchronous backend has returned, which proves there is no detached writer left in the backend.

SaveExecution uses existing TaskScope RAII and CPU/Blocking senders. Neither its completion callback
nor a worker applies a Session baseline; SaveService does that separately on the owner. A foreign store
exception is Unknown, never guessed to be NotPublished.
