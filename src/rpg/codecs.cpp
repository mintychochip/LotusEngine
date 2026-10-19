#include "lotus/rpg/codecs.hpp"

#include <cmath>

#include <nlohmann/json.hpp>

#include "lotus/rpg/components.hpp"
#include "lotus/scene/serialization.hpp"

namespace lotus::rpg {

// Declared before the codec lambdas so nlohmann ADL can see them at the
// point where Inventory and Loot call json.get<Item>().
void to_json(nlohmann::json& j, const Item& item)
{
	j = nlohmann::json{{"name", item.name},
	                   {"description", item.description},
	                   {"type", item.type},
	                   {"value", item.value},
	                   {"stack_size", item.stack_size},
	                   {"quantity", item.quantity},
	                   {"bonus_damage", item.bonus_damage},
	                   {"bonus_defense", item.bonus_defense},
	                   {"bonus_health", item.bonus_health},
	                   {"bonus_mana", item.bonus_mana}};
}

void from_json(const nlohmann::json& j, Item& item)
{
	item.name          = j.value("name", "");
	item.description   = j.value("description", "");
	item.type          = j.value("type", "misc");
	item.value         = j.value("value", 0);
	item.stack_size    = j.value("stack_size", 1);
	item.quantity      = j.value("quantity", 1);
	item.bonus_damage  = j.value("bonus_damage", 0);
	item.bonus_defense = j.value("bonus_defense", 0);
	item.bonus_health  = j.value("bonus_health", 0);
	item.bonus_mana    = j.value("bonus_mana", 0);
}

namespace {

double tidy(float value)
{
	return std::round(static_cast<double>(value) * 1e6) / 1e6;
}

void add_codecs()
{
	register_component_codec(
	        "Stats",
	        [](const Registry& r, Entity e, nlohmann::json& j) {
		        const auto* value = r.try_get<Stats>(e);
		        if (!value) {
			        return false;
		        }
		        j = nlohmann::json{{"level", value->level},
		                           {"xp", value->xp},
		                           {"xp_to_next", value->xp_to_next},
		                           {"strength", value->strength},
		                           {"dexterity", value->dexterity},
		                           {"intelligence", value->intelligence},
		                           {"vitality", value->vitality}};
		        return true;
	        },
	        [](Registry& r, Entity e, const nlohmann::json& j) {
		        Stats s;
		        s.level        = j.value("level", 1);
		        s.xp           = j.value("xp", 0);
		        s.xp_to_next   = j.value("xp_to_next", 100);
		        s.strength     = j.value("strength", 10);
		        s.dexterity    = j.value("dexterity", 10);
		        s.intelligence = j.value("intelligence", 10);
		        s.vitality     = j.value("vitality", 10);
		        r.emplace_or_replace<Stats>(e, s);
	        });
	register_component_codec(
	        "Health",
	        [](const Registry& r, Entity e, nlohmann::json& j) {
		        const auto* value = r.try_get<Health>(e);
		        if (!value) {
			        return false;
		        }
		        j = nlohmann::json{{"current", value->current},
		                           {"max", value->max},
		                           {"is_alive", value->is_alive}};
		        return true;
	        },
	        [](Registry& r, Entity e, const nlohmann::json& j) {
		        Health h;
		        h.current  = j.value("current", 100);
		        h.max      = j.value("max", 100);
		        h.is_alive = j.value("is_alive", true);
		        r.emplace_or_replace<Health>(e, h);
	        });
	register_component_codec(
	        "Mana",
	        [](const Registry& r, Entity e, nlohmann::json& j) {
		        const auto* value = r.try_get<Mana>(e);
		        if (!value) {
			        return false;
		        }
		        j = nlohmann::json{{"current", value->current},
		                           {"max", value->max}};
		        return true;
	        },
	        [](Registry& r, Entity e, const nlohmann::json& j) {
		        Mana m;
		        m.current = j.value("current", 50);
		        m.max     = j.value("max", 50);
		        r.emplace_or_replace<Mana>(e, m);
	        });
	register_component_codec(
	        "Inventory",
	        [](const Registry& r, Entity e, nlohmann::json& j) {
		        const auto* value = r.try_get<Inventory>(e);
		        if (!value) {
			        return false;
		        }
		        j["gold"]  = value->gold;
		        j["items"] = value->items;
		        return true;
	        },
	        [](Registry& r, Entity e, const nlohmann::json& j) {
		        Inventory inv;
		        inv.gold = j.value("gold", 0);
		        if (j.contains("items") && j.at("items").is_array()) {
			        for (const auto& item_j : j.at("items")) {
				        inv.items.push_back(item_j.get<Item>());
			        }
		        }
		        r.emplace_or_replace<Inventory>(e, inv);
	        });
	register_component_codec(
	        "Combat",
	        [](const Registry& r, Entity e, nlohmann::json& j) {
		        const auto* value = r.try_get<Combat>(e);
		        if (!value) {
			        return false;
		        }
		        j = nlohmann::json{
		                {"damage", value->damage},
		                {"defense", value->defense},
		                {"attack_cooldown", tidy(value->attack_cooldown)},
		                {"cooldown_timer", tidy(value->cooldown_timer)},
		                {"is_in_combat", value->is_in_combat},
		                {"aggro_range", tidy(value->aggro_range)},
		                {"attack_range", tidy(value->attack_range)}};
		        return true;
	        },
	        [](Registry& r, Entity e, const nlohmann::json& j) {
		        Combat c;
		        c.damage          = j.value("damage", 10);
		        c.defense         = j.value("defense", 0);
		        c.attack_cooldown = j.value("attack_cooldown", 1.0f);
		        c.cooldown_timer  = j.value("cooldown_timer", 0.0f);
		        c.is_in_combat    = j.value("is_in_combat", false);
		        c.aggro_range     = j.value("aggro_range", 8.0f);
		        c.attack_range    = j.value("attack_range", 2.0f);
		        r.emplace_or_replace<Combat>(e, c);
	        });
	register_component_codec(
	        "NPC",
	        [](const Registry& r, Entity e, nlohmann::json& j) {
		        const auto* value = r.try_get<NPC>(e);
		        if (!value) {
			        return false;
		        }
		        j = nlohmann::json{{"name", value->name},
		                           {"role", value->role},
		                           {"faction", value->faction},
		                           {"is_interactable", value->is_interactable}};
		        return true;
	        },
	        [](Registry& r, Entity e, const nlohmann::json& j) {
		        NPC n;
		        n.name            = j.value("name", "");
		        n.role            = j.value("role", "villager");
		        n.faction         = j.value("faction", "");
		        n.is_interactable = j.value("is_interactable", true);
		        r.emplace_or_replace<NPC>(e, n);
	        });
	register_component_codec(
	        "Loot",
	        [](const Registry& r, Entity e, nlohmann::json& j) {
		        const auto* value = r.try_get<Loot>(e);
		        if (!value) {
			        return false;
		        }
		        j["gold"]      = value->gold;
		        j["is_looted"] = value->is_looted;
		        j["items"]     = value->items;
		        return true;
	        },
	        [](Registry& r, Entity e, const nlohmann::json& j) {
		        Loot l;
		        l.gold      = j.value("gold", 0);
		        l.is_looted = j.value("is_looted", false);
		        if (j.contains("items") && j.at("items").is_array()) {
			        for (const auto& item_j : j.at("items")) {
				        l.items.push_back(item_j.get<Item>());
			        }
		        }
		        r.emplace_or_replace<Loot>(e, l);
	        });
	register_component_codec(
	        "Interactable",
	        [](const Registry& r, Entity e, nlohmann::json& j) {
		        const auto* value = r.try_get<Interactable>(e);
		        if (!value) {
			        return false;
		        }
		        j = nlohmann::json{{"radius", tidy(value->radius)},
		                           {"prompt", value->prompt},
		                           {"is_active", value->is_active}};
		        return true;
	        },
	        [](Registry& r, Entity e, const nlohmann::json& j) {
		        Interactable inter;
		        inter.radius    = j.value("radius", 3.0f);
		        inter.prompt    = j.value("prompt", "Press E to interact");
		        inter.is_active = j.value("is_active", true);
		        r.emplace_or_replace<Interactable>(e, inter);
	        });
	register_component_codec(
	        "AI",
	        [](const Registry& r, Entity e, nlohmann::json& j) {
		        const auto* value = r.try_get<AI>(e);
		        if (!value) {
			        return false;
		        }
		        j = nlohmann::json{{"state", static_cast<int>(value->state)},
		                           {"aggro_range", tidy(value->aggro_range)},
		                           {"attack_range", tidy(value->attack_range)},
		                           {"move_speed", tidy(value->move_speed)}};
		        return true;
	        },
	        [](Registry& r, Entity e, const nlohmann::json& j) {
		        AI ai;
		        ai.state        = static_cast<AIState>(j.value("state", 0));
		        ai.aggro_range  = j.value("aggro_range", 10.0f);
		        ai.attack_range = j.value("attack_range", 2.0f);
		        ai.move_speed   = j.value("move_speed", 3.0f);
		        r.emplace_or_replace<AI>(e, ai);
	        });
	register_component_codec(
	        "Dialogue",
	        [](const Registry& r, Entity e, nlohmann::json& j) {
		        const auto* value = r.try_get<Dialogue>(e);
		        if (!value) {
			        return false;
		        }
		        j = nlohmann::json{{"npc_name", value->npc_name},
		                           {"lines", value->lines},
		                           {"current_line", value->current_line},
		                           {"is_active", value->is_active}};
		        return true;
	        },
	        [](Registry& r, Entity e, const nlohmann::json& j) {
		        Dialogue d;
		        d.npc_name = j.value("npc_name", "");
		        if (j.contains("lines") && j.at("lines").is_array()) {
			        for (const auto& line : j.at("lines")) {
				        d.lines.push_back(line.get<std::string>());
			        }
		        }
		        d.current_line = j.value("current_line", 0);
		        d.is_active    = j.value("is_active", false);
		        r.emplace_or_replace<Dialogue>(e, d);
	        });
	register_component_codec(
	        "Quest",
	        [](const Registry& r, Entity e, nlohmann::json& j) {
		        const auto* value = r.try_get<Quest>(e);
		        if (!value) {
			        return false;
		        }
		        nlohmann::json objectives = nlohmann::json::array();
		        for (const QuestObjective& obj : value->objectives) {
			        objectives.push_back(
			                {{"description", obj.description},
			                 {"required", obj.required},
			                 {"current", obj.current}});
		        }
		        j = nlohmann::json{{"id", value->id},
		                           {"name", value->name},
		                           {"description", value->description},
		                           {"state", static_cast<int>(value->state)},
		                           {"objectives", objectives},
		                           {"xp_reward", value->xp_reward},
		                           {"gold_reward", value->gold_reward},
		                           {"item_rewards", value->item_rewards}};
		        return true;
	        },
	        [](Registry& r, Entity e, const nlohmann::json& j) {
		        Quest q;
		        q.id          = j.value("id", "");
		        q.name        = j.value("name", "");
		        q.description = j.value("description", "");
		        q.state       = static_cast<QuestState>(j.value("state", 1));
		        q.xp_reward   = j.value("xp_reward", 0);
		        q.gold_reward = j.value("gold_reward", 0);
		        if (j.contains("objectives") && j.at("objectives").is_array()) {
			        for (const auto& obj_j : j.at("objectives")) {
				        QuestObjective obj;
				        obj.description =
				                obj_j.value("description", "");
				        obj.required = obj_j.value("required", 1);
				        obj.current  = obj_j.value("current", 0);
				        q.objectives.push_back(obj);
			        }
		        }
		        if (j.contains("item_rewards") &&
		            j.at("item_rewards").is_array()) {
			        for (const auto& item_j : j.at("item_rewards")) {
				        q.item_rewards.push_back(item_j.get<Item>());
			        }
		        }
		        r.emplace_or_replace<Quest>(e, q);
	        });
}

} // namespace

void register_component_codecs()
{
	static bool once = false;
	if (once) {
		return;
	}
	once = true;
	add_codecs();
}

} // namespace lotus::rpg
