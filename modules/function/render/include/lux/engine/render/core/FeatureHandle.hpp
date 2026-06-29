#pragma once
/**
 * @file FeatureHandle.hpp
 * @brief Strong-typed handles for render features and views.
 */

#include <cstdint>
#include <compare>
#include <limits>
#include <lux/cxx/container/SlotMap.hpp>   // lux::cxx::SlotKey

namespace lux::render
{
    /// Tag for the generational feature handle.
    struct FeatureTag {};

    /// Generational feature handle (index + generation). A reused feature slot
    /// bumps its generation, so a stale FeatureHandle is rejected by the scene's
    /// SlotKeyAutoSparseSet lookup instead of aliasing a different feature.
    /// Trivially copyable → serializes across the in-process comm channel (8 bytes).
    /// Members: .index / .gen / .valid().
    using FeatureHandle = lux::cxx::SlotKey<FeatureTag>;

    /// Tag for the generational view handle.
    struct ViewTag {};

    /// Generational view handle (index + generation). A reused view slot bumps its
    /// generation, so a stale ViewHandle (held by a client across a view
    /// remove+recreate) is rejected by the scene's SlotKeyAutoSparseSet lookup
    /// instead of aliasing a different view. Trivially copyable → serializes across
    /// the in-process comm channel (8 bytes). Members: .index / .gen / .valid().
    using ViewHandle = lux::cxx::SlotKey<ViewTag>;
} // namespace lux::render
