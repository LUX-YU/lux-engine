# EC1 installed primitives

Configure against an installed SDK, with `EC1_MODE=HEADLESS` or `EC1_MODE=WINDOW`.
Neither mode imports a concrete tool UI, builtin composition or Application.

- HEADLESS reads a fixed project source on the Process blocking scheduler, installs a real
  Material session, edits and replays its history, publishes through SaveExecution and reads
  the saved source back. No renderer, window, or alternate save implementation is used.
- WINDOW constructs a detached Pane with ordinary Layout/Label controls, registers its factory,
  mounts, focuses and detaches it through the real Root ownership protocol. It has no content association.

`editor-p11` separately exercises an actual extension DLL: the current V10 capability contract,
missing capability and wrong-thread rejection, shared SessionStore access, free windows and
external code lifetime through the last command, control, save result and weak control block.
These are development consumers; final qualification must rebuild them against the SDK from
the implementation SHA being qualified. Historical EC1 evidence keeps its original SHA and ABI.
