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
#include "../xrEngine/SkeletonMotions.h"

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
// distance between the hands: about 1.5 m in a T-pose (the actor's motions do not move this model's bones),
// well under a metre in any pose the motions give; -1 when the model has no such bones. Read from the bones as the
// last frame drew them: calculating them here, outside the frame, would change what is measured
static float coop_hand_span(IKinematics* K)
{
	u16 left = K->LL_BoneID("bip01_l_hand"), right = K->LL_BoneID("bip01_r_hand");
	if (left == BI_NONE || right == BI_NONE)
		return -1.f;
	return K->LL_GetTransform(left).c.distance_to(K->LL_GetTransform(right).c);
}

bool CActor::coop_net_animated() const
{
	if (!(m_coop_updates > 0 && smart_cast<IKinematicsAnimated*>(Visual()) && m_current_legs.valid() &&
		m_current_legs_blend && m_current_legs_blend->blendAmount > 0.f))
		return false;
	return coop_hand_span(smart_cast<IKinematics*>(Visual())) < 1.2f;
}

// which motion library a cycle comes from and which part of this model's skeleton it plays on
static void coop_motion_info(IKinematicsAnimated* KA, const MotionID& id, string128& out)
{
	if (!id.valid())
	{
		xr_strcpy(out, "-");
		return;
	}
	CMotionDef* def = KA->LL_GetMotionDef(id);
	shared_motions& slot = const_cast<shared_motions&>(KA->LL_MotionsSlot(id.slot));
	const CPartDef* own = def && def->bone_or_part != BI_NONE ? slot.partition()->part(def->bone_or_part) : NULL;
	LPCSTR name = "?";
	for (const auto& it : *slot.motion_map())
		if (it.second == id.idx)
		{
			name = it.first.c_str();
			break;
		}
	xr_sprintf(out, "%s slot%u(%s) part%d(%s) speed=%.2f accrue=%.2f power=%.2f", name, id.slot, slot.id().c_str(),
		def ? int(def->bone_or_part) : -1, own ? own->Name.c_str() : "all", def ? def->Speed() : -1.f,
		def ? def->Accrue() : -1.f, def ? def->Power() : -1.f);
}

void CActor::coop_net_debug(string2048& out) const
{
	// how high a bone stands above the model's origin (the feet): a sunk model has its pelvis near or under zero
	auto coop_bone_height = [this](LPCSTR name) -> float
	{
		IKinematics* K = smart_cast<IKinematics*>(Visual());
		u16 bone = K ? K->LL_BoneID(name) : BI_NONE;
		return bone == BI_NONE ? -9.f : K->LL_GetTransform(bone).c.y;
	};
	xr_sprintf(out, "upd=%u en=%d rdy=%d anim_visual=%d legs=%d torso=%d blend=%.2f mstate=%u hand_span=%.2f pelvis_y=%.2f xform_dy=%.2f", m_coop_updates,
		getEnabled() ? 1 : 0, getReady() ? 1 : 0, smart_cast<IKinematicsAnimated*>(Visual()) ? 1 : 0,
		m_current_legs.valid() ? 1 : 0, m_current_torso.valid() ? 1 : 0,
		m_current_legs_blend ? m_current_legs_blend->blendAmount : -1.f, mstate_real,
		Visual() ? coop_hand_span(smart_cast<IKinematics*>(Visual())) : -1.f, coop_bone_height("bip01_pelvis"),
		XFORM().c.y - Position().y);

	IKinematicsAnimated* KA = smart_cast<IKinematicsAnimated*>(Visual());
	if (!KA)
		return;
	string128 legs, torso;
	coop_motion_info(KA, m_current_legs, legs);
	coop_motion_info(KA, m_current_torso, torso);
	string256 parts = "";
	const CPartition& P = KA->partitions();
	for (u16 i = 0; i < MAX_PARTS; ++i)
		if (const CPartDef* part = P.part(i))
		{
			string64 one;
			xr_sprintf(one, " %u:%s/%u/%u", i, part->Name.c_str(), u32(part->bones.size()), KA->LL_PartBlendsCount(i));
			xr_strcat(parts, one);
		}
	string1024 more;
	xr_sprintf(more, " slots=%u legs=%s torso=%s torso_blend=%.2f/%.2f parts(name/bones/blends):%s", KA->LL_MotionsSlotCount(),
		legs, torso, m_current_torso_blend ? m_current_torso_blend->blendAmount : -1.f,
		m_current_torso_blend ? m_current_torso_blend->timeCurrent : -1.f, parts);
	xr_strcat(out, more);
	// bones whose callback replaces the animation (a physics shell does that for every bone it holds)
	{
		IKinematics* K = smart_cast<IKinematics*>(Visual());
		u32 callbacks = 0, overwrite = 0, types = 0;
		for (u16 b = 0; b < K->LL_BoneCount(); ++b)
		{
			CBoneInstance& bi = K->LL_GetBoneInstance(b);
			if (bi.callback())
			{
				++callbacks;
				types |= 1u << (bi.callback_type() & 31);
			}
			if (bi.callback_overwrite())
				++overwrite;
		}
		u16 arm = K->LL_BoneID("bip01_l_forearm");
		string256 cb;
		xr_sprintf(cb, " bones=%u callbacks=%u overwrite=%u types=%x l_forearm(cb=%d ow=%d visible=%d)", K->LL_BoneCount(),
			callbacks, overwrite, types, arm != BI_NONE && K->LL_GetBoneInstance(arm).callback() ? 1 : 0,
			arm != BI_NONE && K->LL_GetBoneInstance(arm).callback_overwrite() ? 1 : 0,
			arm != BI_NONE && K->LL_GetBoneVisible(arm) ? 1 : 0);
		xr_strcat(out, cb);
	}
	// which bones of this model the legs part drives (a part names its bones by the first model that loaded it)
	if (const CPartDef* legs_part = P.part(0))
	{
		IKinematics* K = smart_cast<IKinematics*>(Visual());
		xr_strcat(out, " legs_bones:");
		for (u32 b : legs_part->bones)
		{
			string64 one;
			xr_sprintf(one, " %s", b < K->LL_BoneCount() ? K->LL_BoneName_dbg(u16(b)) : "?");
			xr_strcat(out, one);
		}
	}
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
