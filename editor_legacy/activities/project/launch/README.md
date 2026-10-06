# Editor process launch

`launchEditor(installation, project_file)` asks the platform process API to start the installed Editor.
Its existing signature, error payload and logical include remain unchanged. The caller owns scheduling;
this library does not create an executor, project, window or second application context.

`ProjectLaunching` owns one accepted Process task and its retained result. Both products resolve the
same declared service. It creates no thread pool; completion writes only its own result. Another
request is rejected until explicit acknowledgment. UI disappearance does not discard a launch fact.
The service factory borrows ExecutionRuntime and the installation path only at creation time.
