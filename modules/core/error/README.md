# Structured errors

`Error` is a 32-byte value: stable FNV-1a canonical type identity and three numeric arguments.
`ErrorRegistry` lives in the core error DLL. Registration copies immutable descriptions; no code,
formatter, string borrow or failure instance is retained. Exact duplicates are idempotent, conflicting
definitions and hash collisions fail. Published addresses remain valid until process shutdown.

Messages use `{0}` through `{2}` and escaped `{{`/`}}`. Argument kinds are unsigned, signed and hexadecimal;
every declared argument must occur in the message. Formatting never calls domain/plugin code.
`makeError` returns an explicit registration error for malformed or conflicting descriptors.
Unknown identities format with their identity and all parameters. Allocation failure is not recovered.

Keep detailed domain failures with their original owner. At a cross-system boundary, map the precise
classification and relevant numeric cause; do not encode temporary registry slots as stable identities.
Registration/query are thread-safe. Register descriptors before unloading their providing module.
