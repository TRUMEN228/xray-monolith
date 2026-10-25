////////////////////////////////////////////////////////////////////////////
//	Module 		: Actor_Coop.cpp
//	Description : Anomaly Co-op: the partner shown as a second actor driven by the network
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "Actor.h"
#include "coop_net_puppet.h"
#include "CharacterPhysicsSupport.h"
#include "PHMovementControl.h"

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
	mstate_real = mstate_wishful = m_coop_mstate;
	NET_Last.o_model = m_coop_angles.x;
	unaffected_r_torso.yaw = m_coop_angles.y;
	unaffected_r_torso.pitch = m_coop_angles.z;
	unaffected_r_torso.roll = 0.f;
	g_sv_Orientate(mstate_real, dt);
	g_Orientate(mstate_real, dt);
	g_SetAnimation(mstate_real);
	mstate_old = mstate_real;
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
