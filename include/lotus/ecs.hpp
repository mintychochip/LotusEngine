#pragma once

/**
 * @file ecs.hpp
 * @brief The engine's entity-component-system facade.
 *
 * EnTT is the single source of truth for entities and components. Engine and
 * game code include this header rather than <entt/entt.hpp> directly, so the
 * dependency stays confined to one place.
 */

#include <entt/entt.hpp>

namespace lotus {

/// A versioned entity handle. A stale handle from a destroyed entity never
/// aliases a recycled one, because the version bits differ.
using Entity = entt::entity;

/// Sentinel for "no entity".
inline constexpr Entity NullEntity{entt::null};

/// The registry that owns entities and their components.
using Registry = entt::registry;

} // namespace lotus
