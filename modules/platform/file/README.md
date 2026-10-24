# File IO

`platform_file` provides synchronous, bounded reads and atomic file publication. It owns no executor or thread;
callers schedule blocking work through their existing execution runtime.

`AtomicFileWriter::create()` exclusively creates a temporary file beside an absolute destination. `write()`
supports streaming, and `seek()` supports header backfill. `commit()` flushes and publishes; destruction removes
an unpublished candidate. `CREATE` never overwrites an existing destination. `REPLACE` never deletes the old
destination before replacement. The caller creates any required parent directories.

Cancellation is observed before publication. Once published, the result cannot be cancellation with no effects.
`PUBLICATION_UNKNOWN` reports a visibility/durability uncertainty after publication; callers must not blindly
retry it as an untouched destination. Flush and filesystem rename guarantees still depend on the actual OS,
filesystem and storage device; this is not a multi-file or universal power-failure transaction.

Scene encoding, asset identity, catalog transactions and UI error policy belong to their own modules. Public
headers contain no platform SDK handle types.
