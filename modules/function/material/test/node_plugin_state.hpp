#pragma once

struct NodePluginState final
{
    int constructed{};
    int destroyed{};
    int clone_returns{};
    bool reject_clone{};
    bool unloaded{};
};
