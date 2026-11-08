////////////////////////////////////////////////////////////////////////////
//	Module 		: script_game_object_script.cpp
//	Created 	: 25.09.2003
//  Modified 	: 29.06.2004
//	Author		: Dmitriy Iassenev
//	Description : XRay Script game object script export
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "coop_link.h"
#include "coop_creatures.h"
#include "coop_items.h"
#include "coop_clock.h"
#include "coop_news.h"
#include "coop_partner.h"
#include "coop_props.h"
#include "coop_update.h"
#include "UIGameSP.h"
#include "ui/UITalkWnd.h"
#include "Level.h"
#include "InventoryOwner.h"
#include "script_game_object.h"
#include "game_object_space.h"
#include "script_ini_file.h"
#include "sight_manager_space.h"
#include "ai/monsters/state_defs.h"
#include "inventory_space.h"

using namespace luabind;

extern class_<CScriptGameObject> script_register_game_object1(class_<CScriptGameObject> &&);
extern class_<CScriptGameObject> script_register_game_object2(class_<CScriptGameObject> &&);
extern class_<CScriptGameObject> script_register_game_object_trader(class_<CScriptGameObject> &&);

// Anomaly Co-op: lets the mod know these exes have its engine side, and which revision
// 1: net_puppet; 2: net_puppet with delay 0 adapts to the link, net_puppet_delay;
// 3: stalker puppets pick gait and leg direction by how they really move, net_puppet_body;
// 4: the partner can be a second actor (section with coop_net_actor = true): net_actor_state/mstate/angles
// 5: the actor partner is drawn whole, holds its weapon and shows its shots: net_actor_fire/reload;
//    hits on it reach the hit callback (friendly fire) and do no damage; net_actor_animated/debug;
//    net_puppet_look: a stalker puppet's head and body follow the sender's look at once
// 6: the actor partner spawns alive (no ragdoll holding its bones in a T-pose): the actor partner is usable
// 7: net_puppet_mental: a stalker puppet is animated calm, alert or panicked as on the sender, not by its own AI;
//    net_weapon_addons: the partner's weapon shows the scope, silencer and grenade launcher it has there
// 8: the link to the partner in the engine, without CoopLauncher: coop_link_host/join/stop/out/in/status,
//    through Steam: coop_link_host_steam/join_steam, coop_link_friends, coop_link_invite; coop_link_take_session;
//    the main menu calls coop_menu.before_main_menu() before it is made;
//    updates from the game: coop_update_check/download/apply/status
// 9: the partner's hand flashlight shines (net_actor_flashlight); no crash on exit after a leaked object
// 10: net_actor_flashlight shows any device the partner holds: in its left hand, the arm bent to hold it;
//     set_character_name; the Steam link outlives a lobby hiccup, a partner's new hello is answered
// 11: net_puppet_strap: a stalker puppet carries its weapon on its back or in its hands as on the sender;
//     the talk window hands its answers to coop_dialog.on_options and takes the partner's with coop_dialog_pick,
//     and opens as a copy of the partner's talk (coop_talk_mirror_*);
//     ActorMenu:coop_refresh_box(id) shows a box or body anew
// 12: the session in the engine (link protocol 8): the world to a guest in its menu, reloads, the host's travel,
//     the guest's game ending with the host's; coop_session_reload(save_first), coop_session_moved(); the engine
//     calls coop_session.engine_before_load(why) and coop_menu.engine_loading() before it loads; the host's creatures
//     go to the guest's copies in the engine (coop_creatures): coop_net_map/unmap(host id, id), coop_net_set_partner,
//     coop_net_target(id); a hit on the partner goes to coop_combat.partner_hit
// 13: items in the engine (coop_items, link protocol 9): takes from boxes, bodies and the ground, drops, puts, the
//     host's box lists and bodies' loot; coop_items_ignore(id, on), coop_items_pause(ms); new items through
//     coop_world.engine_spawn, open windows shown anew through coop_world.engine_refresh; coop_items_shared(on):
//     the host's "shared loot" option (off: boxes and bodies are each player's own copy)
static int coop_engine_version() { return 19; }

// the game script swaps its coop_out.txt / coop_in.txt frames with the engine instead of the launcher
static bool coop_link_host(int port, LPCSTR name, LPCSTR version) { return coop_link::host(port, name, version); }
static bool coop_link_join(LPCSTR address, int port, LPCSTR name, LPCSTR version) { return coop_link::join(address, port, name, version); }
static void coop_link_stop() { coop_link::stop(); }
static void coop_link_out(LPCSTR text) { coop_link::game_out(text); }
static LPCSTR coop_link_in() { return coop_link::game_in(); }
static LPCSTR coop_link_status() { return coop_link::status(); }
static bool coop_link_host_steam(LPCSTR name, LPCSTR version) { return coop_link::host_steam(name, version); }
static bool coop_link_join_steam(LPCSTR lobby, LPCSTR name, LPCSTR version) { return coop_link::join_steam(lobby, name, version); }
static LPCSTR coop_link_friends() { return coop_link::steam_friends(); }
static LPCSTR coop_link_invite() { return coop_link::steam_invite(); }
static bool coop_link_take_session() { return coop_link::take_session(); }
static void coop_session_reload(bool save_first) { coop_link::session_reload(save_first); }
static void coop_session_moved() { coop_link::session_moved(); }
static void coop_net_map(int host_id, int id) { coop_creatures::map((u16)host_id, (u16)id); }
static void coop_net_unmap(int host_id) { coop_creatures::unmap((u16)host_id); }
static void coop_net_set_partner(int id) { coop_creatures::set_partner(id < 0 ? u16(-1) : (u16)id); }
static int coop_net_target(int id) { return coop_creatures::target_of((u16)id); }
static void coop_items_ignore(int id, bool ignore) { coop_items::ignore_box((u16)id, ignore); }
static void coop_items_pause(int ms) { coop_items::pause((u32)_max(ms, 0)); }
static void coop_items_shared(bool shared) { coop_items::set_shared(shared); }
static void coop_news_keep(bool on) { coop_news::keep_local(on); }
static void coop_partner_extras(LPCSTR s) { coop_partner::set_extras(s ? s : ""); }
static void coop_money_shared(bool on) { coop_props::set_shared_money(on); }
static void coop_money_give(int n) { coop_props::give_money(n); }
static void coop_door_scripted(int id) { coop_props::skip_door((u16)id); }
static void coop_friendly_fire(bool on) { coop_props::set_friendly_fire(on); }
static bool coop_items_gift(int id) { return coop_items::gift((u16)id); }
static bool coop_spawn_item(int id, LPCSTR kind) { return coop_items::world_item((u16)id, kind); }
static void coop_spawn_arm(bool on) { coop_props::set_spawns_armed(on); }
static void coop_info_ready(bool on) { coop_props::set_info_ready(on); }
static void coop_talk_end() { coop_props::talk_end(); }
static void coop_info_skip(LPCSTR info) { coop_props::skip_info(info); }
// the tests: a deal as CTrade::TransferItem tells it (a script cannot trade)
static void coop_test_traded(int trader, int item, LPCSTR section, bool bought) { coop_props::traded((u16)trader, (u16)item, section, bought); }
static int coop_news_count(LPCSTR text) { return coop_news::count(text); }
static void coop_world_jumped() { coop_clock::weather_jumped(); }
static void coop_world_state(LPCSTR s) { coop_clock::set_state(s ? s : ""); }
static void coop_update_check(LPCSTR current) { coop_update::check(current); }
static void coop_update_download() { coop_update::download(); }
static bool coop_update_apply() { return coop_update::apply(); }
static LPCSTR coop_update_status() { return coop_update::status(); }
// the partner chose an answer in its copy of this player's talk window
static CUITalkWnd* coop_talk_wnd()
{
	CUIGameSP* ui = smart_cast<CUIGameSP*>(CurrentGameUI());
	return ui ? ui->TalkMenu : nullptr;
}
static void coop_dialog_pick(LPCSTR id)
{
	if (CUITalkWnd* w = coop_talk_wnd())
		w->CoopPick(id);
}
// this player's talk window as a copy of the partner's talk with the same NPC
static bool coop_talk_mirror_open(u16 npc_id)
{
	CUITalkWnd* w = coop_talk_wnd();
	CInventoryOwner* npc = smart_cast<CInventoryOwner*>(Level().Objects.net_Find(npc_id));
	return w && npc && w->CoopMirrorOpen(npc);
}
static void coop_talk_mirror_line(LPCSTR speaker, LPCSTR text, bool ours)
{
	if (CUITalkWnd* w = coop_talk_wnd())
		w->CoopMirrorLine(speaker, text, ours);
}
static void coop_talk_mirror_options(LPCSTR list)
{
	if (CUITalkWnd* w = coop_talk_wnd())
		w->CoopMirrorOptions(list);
}
static void coop_talk_mirror_close()
{
	if (CUITalkWnd* w = coop_talk_wnd())
		w->CoopMirrorClose();
}
static void coop_talk_mirror_switch(LPCSTR kind)
{
	if (CUITalkWnd* w = coop_talk_wnd())
		w->CoopMirrorSwitch(kind);
}
static bool coop_talk_mirror_shown()
{
	CUITalkWnd* w = coop_talk_wnd();
	return w && w->CoopMirror() && w->IsShown();
}

#pragma optimize("s",on)
void CScriptGameObject::script_register(lua_State* L)
{
	class_<CScriptGameObject> instance("game_object");

	module(L)
	[
		class_<CSightParams>("CSightParams")
		.enum_("bla-bla")
		[
			value("eSightTypeCurrentDirection", int(SightManager::eSightTypeCurrentDirection)),
			value("eSightTypePathDirection", int(SightManager::eSightTypePathDirection)),
			value("eSightTypeDirection", int(SightManager::eSightTypeDirection)),
			value("eSightTypePosition", int(SightManager::eSightTypePosition)),
			value("eSightTypeObject", int(SightManager::eSightTypeObject)),
			value("eSightTypeCover", int(SightManager::eSightTypeCover)),
			value("eSightTypeSearch", int(SightManager::eSightTypeSearch)),
			value("eSightTypeLookOver", int(SightManager::eSightTypeLookOver)),
			value("eSightTypeCoverLookOver", int(SightManager::eSightTypeCoverLookOver)),
			value("eSightTypeFireObject", int(SightManager::eSightTypeFireObject)),
			value("eSightTypeFirePosition", int(SightManager::eSightTypeFirePosition)),
			value("eSightTypeAnimationDirection", int(SightManager::eSightTypeAnimationDirection)),
			value("eSightTypeDummy", int(SightManager::eSightTypeDummy))
		]
		.def(constructor<>())
		.def_readonly("m_object", &CSightParams::m_object)
		.def_readonly("m_vector", &CSightParams::m_vector)
		.def_readonly("m_sight_type", &CSightParams::m_sight_type),

		script_register_game_object2(
			script_register_game_object1(
				script_register_game_object_trader(std::move(instance))
			)
		),

		class_<enum_exporter<EInventorySlots>>("inventory")
		.enum_("slot")
		[
			value("no_active", int(NO_ACTIVE_SLOT)),
			value("knife", int(KNIFE_SLOT)),
			value("slot_2", int(INV_SLOT_2)),
			value("slot_3", int(INV_SLOT_3)),
			value("grenade", int(GRENADE_SLOT)),
			value("binocular", int(BINOCULAR_SLOT)),
			value("bolt", int(BOLT_SLOT)),
			value("outfit", int(OUTFIT_SLOT)),
			value("pda", int(PDA_SLOT)),
			value("detector", int(DETECTOR_SLOT)),
			value("torch", int(TORCH_SLOT)),
			value("artefact", int(ARTEFACT_SLOT)),
			value("helmet", int(HELMET_SLOT)),
			value("backpack", int(BACKPACK_SLOT)),
#ifdef MORE_INVENTORY_SLOTS
			value("custom_1", int(CUSTOM_SLOT_1)),
			value("custom_2", int(CUSTOM_SLOT_2)),
			value("custom_3", int(CUSTOM_SLOT_3)),
			value("custom_4", int(CUSTOM_SLOT_4)),
			value("custom_5", int(CUSTOM_SLOT_5)),
#endif
			value("last", int(LAST_SLOT))
		],

		class_<enum_exporter<GameObject::ECallbackType>>("callback")
		.enum_("callback_types")
		[
			value("trade_start", int(GameObject::eTradeStart)),
			value("trade_stop", int(GameObject::eTradeStop)),
			value("trade_sell_buy_item", int(GameObject::eTradeSellBuyItem)),
			value("trade_perform_operation", int(GameObject::eTradePerformTradeOperation)),
			value("trader_global_anim_request", int(GameObject::eTraderGlobalAnimationRequest)),
			value("trader_head_anim_request", int(GameObject::eTraderHeadAnimationRequest)),
			value("trader_sound_end", int(GameObject::eTraderSoundEnd)),
			value("zone_enter", int(GameObject::eZoneEnter)),
			value("zone_exit", int(GameObject::eZoneExit)),
			value("level_border_exit", int(GameObject::eExitLevelBorder)),
			value("level_border_enter", int(GameObject::eEnterLevelBorder)),
			value("death", int(GameObject::eDeath)),
			value("patrol_path_in_point", int(GameObject::ePatrolPathInPoint)),
			value("inventory_pda", int(GameObject::eInventoryPda)),
			value("inventory_info", int(GameObject::eInventoryInfo)),
			value("article_info", int(GameObject::eArticleInfo)),
			value("use_object", int(GameObject::eUseObject)),
			value("hit", int(GameObject::eHit)),
			value("sound", int(GameObject::eSound)),
			value("action_removed", int(GameObject::eActionTypeRemoved)),
			value("action_movement", int(GameObject::eActionTypeMovement)),
			value("action_watch", int(GameObject::eActionTypeWatch)),
			value("action_animation", int(GameObject::eActionTypeAnimation)),
			value("action_sound", int(GameObject::eActionTypeSound)),
			value("action_particle", int(GameObject::eActionTypeParticle)),
			value("action_object", int(GameObject::eActionTypeObject)),
			value("actor_sleep", int(GameObject::eActorSleep)),
			value("helicopter_on_point", int(GameObject::eHelicopterOnPoint)),
			value("helicopter_on_hit", int(GameObject::eHelicopterOnHit)),
			value("on_item_take", int(GameObject::eOnItemTake)),
			value("on_item_drop", int(GameObject::eOnItemDrop)),
			value("script_animation", int(GameObject::eScriptAnimation)),
			value("task_state", int(GameObject::eTaskStateChange)),
			value("take_item_from_box", int(GameObject::eInvBoxItemTake)),
			value("weapon_no_ammo", int(GameObject::eWeaponNoAmmoAvailable)),

			//Alundaio:
			value("hud_animation_end", int(GameObject::eActorHudAnimationEnd)),
			//AVO: custom callbacks
			// input
#ifdef INPUT_CALLBACKS
			value("key_press", int(GameObject::eKeyPress)),
			value("key_release", int(GameObject::eKeyRelease)),
			value("key_hold", int(GameObject::eKeyHold)),
#endif
#ifdef MOUSE_INPUT_CALLBACKS
                value("mouse_move",                 int(GameObject::eMouseMove)),
                value("mouse_wheel",                int(GameObject::eMouseWheel)),
#endif
			// actor
#ifdef ACTOR_BEFORE_DEATH_CALLBACK
			value("actor_before_death", int(GameObject::eActorBeforeDeath)),
#endif
			// vehicle
#ifdef ENABLE_CAR
			value("on_attach_vehicle", int(GameObject::eAttachVehicle)),
			value("on_detach_vehicle", int(GameObject::eDetachVehicle)),
			value("on_use_vehicle", int(GameObject::eUseVehicle)),
#endif
#ifdef EXTENDED_WEAPON_CALLBACKS
			//weapon
			value("weapon_fired", int(GameObject::eOnWeaponFired)),
			value("weapon_jammed", int(GameObject::eOnWeaponJammed)),
			value("weapon_zoom_in", int(GameObject::eOnWeaponZoomIn)),
			value("weapon_zoom_out", int(GameObject::eOnWeaponZoomOut)),
			value("weapon_magazine_empty", int(GameObject::eOnWeaponMagazineEmpty)),
			value("weapon_lowered", int(GameObject::eOnWeaponLowered)),
			value("weapon_raised", int(GameObject::eOnWeaponRaised)),
#endif
#ifdef EXTENDED_ITEM_CALLBACKS
			// inventory
			value("item_to_belt", int(GameObject::eItemToBelt)),
			value("item_to_slot", int(GameObject::eItemToSlot)),
			value("item_to_ruck", int(GameObject::eItemToRuck)),
#endif
#ifdef NET_SPAWN_AFTER_CALLBACKS
			value("net_spawn_after", int(GameObject::eNetSpawnAfter)),
#endif
			//-AVO
			value("on_foot_step", int(GameObject::eOnFootStep)),

			value("map_location_added", int(GameObject::eMapLocationAdded))
		],

		// Attack substates the monster_on_combat_action callback can propose in flags.substate.
		// These are the states the shared monster and group attack selectors register
		// (monster_state_attack_inline.h, group_state_attack_inline.h); any other EMonsterState
		// is ignored and the vanilla selection runs.
		class_<enum_exporter<EMonsterState>>("monster_state")
		.enum_("substate")
		[
			value("eStateAttack_Run", int(eStateAttack_Run)),
			value("eStateAttack_Melee", int(eStateAttack_Melee)),
			value("eStateAttack_RunAttack", int(eStateAttack_RunAttack)),
			value("eStateAttack_RunAway", int(eStateAttack_RunAway)),
			value("eStateAttack_FindEnemy", int(eStateAttack_FindEnemy)),
			value("eStateAttack_Steal", int(eStateAttack_Steal)),
			value("eStateAttack_AttackHidden", int(eStateAttack_AttackHidden)),
			value("eStateAttack_ControlFire", int(eStateAttack_ControlFire)),
			value("eStateAttackCamp", int(eStateAttackCamp)),
			value("eStateAttack_MoveToHomePoint", int(eStateAttack_MoveToHomePoint)),
			value("eStateAttack_Attack_On_Run", int(eStateAttack_Attack_On_Run)),
			value("eStateCustom", int(eStateCustom))
		],

		def("buy_condition", (void (*)(CScriptIniFile*, LPCSTR))(&::buy_condition)),
		def("buy_condition", (void (*)(float, float))(&::buy_condition)),
		def("sell_condition", (void (*)(CScriptIniFile*, LPCSTR))(&::sell_condition)),
		def("sell_condition", (void (*)(float, float))(&::sell_condition)),
		def("show_condition", &::show_condition),
		def("coop_engine_version", &::coop_engine_version),
		def("coop_link_host", &::coop_link_host),
		def("coop_link_join", &::coop_link_join),
		def("coop_link_stop", &::coop_link_stop),
		def("coop_link_out", &::coop_link_out),
		def("coop_link_in", &::coop_link_in),
		def("coop_link_status", &::coop_link_status),
		def("coop_link_host_steam", &::coop_link_host_steam),
		def("coop_link_join_steam", &::coop_link_join_steam),
		def("coop_link_friends", &::coop_link_friends),
		def("coop_link_invite", &::coop_link_invite),
		def("coop_link_take_session", &::coop_link_take_session),
		def("coop_session_reload", &::coop_session_reload),
		def("coop_session_moved", &::coop_session_moved),
		def("coop_net_map", &::coop_net_map),
		def("coop_net_unmap", &::coop_net_unmap),
		def("coop_net_set_partner", &::coop_net_set_partner),
		def("coop_net_target", &::coop_net_target),
		def("coop_items_ignore", &::coop_items_ignore),
		def("coop_items_pause", &::coop_items_pause),
		def("coop_items_shared", &::coop_items_shared),
		def("coop_world_jumped", &::coop_world_jumped),
		def("coop_news_count", &::coop_news_count),
		def("coop_news_keep", &::coop_news_keep),
		def("coop_partner_extras", &::coop_partner_extras),
		def("coop_money_shared", &::coop_money_shared),
		def("coop_money_give", &::coop_money_give),
		def("coop_door_scripted", &::coop_door_scripted),
		def("coop_friendly_fire", &::coop_friendly_fire),
		def("coop_items_gift", &::coop_items_gift),
		def("coop_spawn_item", &::coop_spawn_item),
		def("coop_spawn_arm", &::coop_spawn_arm),
		def("coop_info_ready", &::coop_info_ready),
		def("coop_talk_end", &::coop_talk_end),
		def("coop_info_skip", &::coop_info_skip),
		def("coop_test_traded", &::coop_test_traded),
		def("coop_world_state", &::coop_world_state),
		def("coop_update_check", &::coop_update_check),
		def("coop_update_download", &::coop_update_download),
		def("coop_update_apply", &::coop_update_apply),
		def("coop_update_status", &::coop_update_status),
		def("coop_dialog_pick", &::coop_dialog_pick),
		def("coop_talk_mirror_open", &::coop_talk_mirror_open),
		def("coop_talk_mirror_line", &::coop_talk_mirror_line),
		def("coop_talk_mirror_options", &::coop_talk_mirror_options),
		def("coop_talk_mirror_close", &::coop_talk_mirror_close),
		def("coop_talk_mirror_switch", &::coop_talk_mirror_switch),
		def("coop_talk_mirror_shown", &::coop_talk_mirror_shown)
	];
}
