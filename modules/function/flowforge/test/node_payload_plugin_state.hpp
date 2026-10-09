#pragma once

struct FlowPayloadPluginState final
{
    int constructed{};
    int destroyed{};
    int clone_calls{};
    bool reject_clone{};
    bool unloaded{};
};
