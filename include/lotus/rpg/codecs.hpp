#pragma once

namespace lotus::rpg {

/// Registers RPG component codecs on the process-global scene table.
/// Idempotent. The engine serializer does not know these types; the game
/// calls this before the first scene load.
void register_component_codecs();

} // namespace lotus::rpg
