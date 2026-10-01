# Task observation

Application composition owns one TaskMonitor per ExecutionRuntime. It installs and releases the sole
observer, coalesces revision changes during Runtime dispatch, and emits changed only after dispatch
returns. Runtime retains all task facts and cancellation decisions. TaskView and legacy TaskPane borrow
this provider and share one immutable snapshot per revision. Their destruction cannot cancel tasks or
remove the application observer. Notifications may be lost; revision checks resynchronize consumers.
