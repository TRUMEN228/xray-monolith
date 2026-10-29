////////////////////////////////////////////////////////////////////////////
//	Module 		: stalker_animation_manager_impl.h
//	Created 	: 16.11.2003
//  Modified 	: 16.11.2005
//	Author		: Dmitriy Iassenev
//	Description : Stalker animation manager inline functions
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "CharacterPhysicsSupport.h"

IC CStalkerAnimationManager::EBodyState CStalkerAnimationManager::body_state() const
{
	// Anomaly Co-op: a network puppet crouches when its sender does
	u8 net_body = object().net_puppet_body();
	if (net_body != 255)
		return (EBodyState(net_body));

	return (object().movement().body_state());
}

IC MonsterSpace::EMentalState CStalkerAnimationManager::mental_state() const
{
	// Anomaly Co-op: a network puppet looks as calm or alert as on the sender
	u8 net_mental = object().net_puppet_mental();
	if (net_mental <= MonsterSpace::eMentalStatePanic)
	{
		// there are no calm animations for crouching (the two states may come a moment apart)
		if (net_mental == MonsterSpace::eMentalStateFree && body_state() != eBodyStateStand)
			return (MonsterSpace::eMentalStateDanger);
		return (MonsterSpace::EMentalState(net_mental));
	}

	return (object().movement().mental_state());
}

IC void CStalkerAnimationManager::fill_object_info()
{
	CInventoryItem* item = object().inventory().ActiveItem();
	VERIFY(item);
	m_weapon = smart_cast<CWeapon*>(item);
	m_missile = smart_cast<CMissile*>(item);
}

IC bool CStalkerAnimationManager::strapped() const
{
	VERIFY(m_weapon);
	return (object().CObjectHandler::weapon_strapped(m_weapon));
}

IC u32 CStalkerAnimationManager::object_slot() const
{
	if (m_weapon)
		return (m_weapon->animation_slot());

	if (m_missile)
		return (m_missile->animation_slot());

	return (0);
}
