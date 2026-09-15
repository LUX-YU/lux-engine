#pragma once

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>

// Test-only batch. Normal verification does not run this optional measurement.
template <class Document, class Edit> void measureEdits(const char *name, Document &document, Edit edit)
{
    if (!std::getenv("D2_MEASURE_EDITS"))
    {
        return;
    }
    const auto initial = document.historyView()->history.current;
    std::chrono::nanoseconds elapsed{};
    for (unsigned index{}; index < 120; ++index)
    {
        const auto begin = std::chrono::steady_clock::now();
        assert(edit(index));
        assert(document.undo() && document.redo() && document.undo());
        if (index >= 20)
        {
            elapsed += std::chrono::steady_clock::now() - begin;
        }
        assert(document.historyView()->history.current == initial);
    }
    std::printf("MEASURE graph=%s warmup=20 edits=100 undo=200 redo=100 active_us=%.3f retained=%zu\n", name,
                std::chrono::duration<double, std::micro>(elapsed).count(),
                document.historyView()->history.charged_retained_bytes);
}
