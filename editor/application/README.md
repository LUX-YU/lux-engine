# Application

`editor_launch` owns the existing process launch function used by Launcher and product commands.
It contains no project creation UI or author model. Runtime/service assembly remains in the
formal integration harness; it is not installed as another product. The existing desktop product
still uses its explicitly retained app/context until the P12 switch.
