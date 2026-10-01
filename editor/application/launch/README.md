# Editor process launch

`launchEditor(installation, project_file)` asks the platform process API to start the installed Editor.
Its existing signature, error payload and logical include remain unchanged. The caller owns scheduling;
this library does not create an executor, project, window or second application context.
