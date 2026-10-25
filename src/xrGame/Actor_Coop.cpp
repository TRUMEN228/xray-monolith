////////////////////////////////////////////////////////////////////////////
//	Module 		: Actor_Coop.cpp
//	Description : Anomaly Co-op: the partner shown as a second actor driven by the network
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "Actor.h"
#include "coop_net_puppet.h"
#include "CharacterPhysicsSupport.h"
#include "PHMovementControl.h"
#include "Inventory.h"
#include "Weapon.h"
#include "xr_level_controller.h"
#include "../Include/xrRender/Kinematics.h"
#include "../Include/xrRender/KinematicsAnimated.h"
#include "../xrEngine/motion.h"

void CActor::coop_net_push(const Fvector& position, u32 remote_time)
{
	if (m_coop_net)
		m_coop_net->push(position, remote_time);
}

void CActor::coop_net_set_state(u32 mstate, const Fvector& angles)
{
	m_coop_mstate = mstate;
	m_coop_angles = angles;
}

Fvector CActor::coop_net_angles() const
{
	return Fvector().set(r_model_yaw, unaffected_r_torso.yaw, unaffected_r_torso.pitch);
}

// scheduled: movement flags and angles as the partner has them, then the engine picks the animation
void CActor::coop_net_update(float dt)
{
	// it never dies of hunger, radiation or bleeding here: the partner's own game decides
	SetfHealth(1.f);
	++m_coop_updates;
	mstate_real = mstate_wishful = m_coop_mstate;
	NET_Last.o_model = m_coop_angles.x;
	unaffected_r_torso.yaw = m_coop_angles.y;
	unaffected_r_torso.pitch = m_coop_angles.z;
	unaffected_r_torso.roll = 0.f;
	g_sv_Orientate(mstate_real, dt);
	g_Orientate(mstate_real, dt);
	g_SetAnimation(mstate_real);
	mstate_old = mstate_real;
	coop_net_weapon();
}

// shots are shown only: the hits they make here do not count, the partner's game sends its own
void CActor::coop_net_weapon()
{
	CWeapon* weapon = smart_cast<CWeapon*>(inventory().ActiveItem());
	if (!weapon)
		return;
	if (m_coop_fire)
	{
		if (weapon->GetAmmoElapsed() < 1 && weapon->GetState() != CWeapon::eReload)
			weapon->SetAmmoElapsed(weapon->GetAmmoMagSize());
		if (!weapon->IsWorking())
			weapon->Action(kWPN_FIRE, CMD_START);
	}
	else if (weapon->IsWorking())
		weapon->Action(kWPN_FIRE, CMD_STOP);
}

// the legs play a cycle: the model has the actor's motions and the scheduler drives them
bool CActor::coop_net_animated() const
{
	return m_coop_updates > 0 && smart_cast<IKinematicsAnimated*>(Visual()) && m_current_legs.valid() &&
		m_current_legs_blend && m_current_legs_blend->blendAmount > 0.f;
}

void CActor::coop_net_debug(string256& out) const
{
	xr_sprintf(out, "upd=%u en=%d rdy=%d anim_visual=%d legs=%d torso=%d blend=%.2f mstate=%u", m_coop_updates,
		getEnabled() ? 1 : 0, getReady() ? 1 : 0, smart_cast<IKinematicsAnimated*>(Visual()) ? 1 : 0,
		m_current_legs.valid() ? 1 : 0, m_current_torso.valid() ? 1 : 0,
		m_current_legs_blend ? m_current_legs_blend->blendAmount : -1.f, mstate_real);
}

void CActor::coop_net_reload()
{
	if (CWeapon* weapon = smart_cast<CWeapon*>(inventory().ActiveItem()))
		weapon->Action(kWPN_RELOAD, CMD_START);
}

// every frame: where the partner is now, by the network
void CActor::coop_net_place()
{
	Fvector position;
	if (!g_Alive() || !m_coop_net->sample(position))
		return;
	if (CPHMovementControl* movement = character_physics_support()->movement())
		movement->SetPosition(position);
	Position().set(position);
}
