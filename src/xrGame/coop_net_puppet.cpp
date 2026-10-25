////////////////////////////////////////////////////////////////////////////
//	Module 		: coop_net_puppet.cpp
//	Description : Anomaly Co-op: a monster whose position comes from the network
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "coop_net_puppet.h"

namespace
{
	const float teleport_distance = 15.f; // a jump this long between two keys is a real teleport
	const u32 max_extrapolation = 250; // ms past the newest key before the puppet stops and waits
	const u32 max_keys = 64;
	const float min_delay = 50.f, max_delay = 500.f, start_delay = 200.f;
	const float delay_rate = 0.1f; // the delay changes by at most this share of real time: playback speeds up or slows down by 10%
}

CCoopNetPuppet::CCoopNetPuppet(u32 delay) : m_fixed_delay(delay)
{
	reset();
}

void CCoopNetPuppet::reset()
{
	m_keys.clear();
	m_delay = m_fixed_delay ? float(m_fixed_delay) : start_delay;
	m_late = 0.f;
	m_interval = 100.f;
	m_offset = 0;
	m_offset_valid = false;
	m_last.set(0.f, 0.f, 0.f);
	m_last_time = 0;
	m_last_valid = false;
	m_speed = 0.f;
}

void CCoopNetPuppet::push(const Fvector& p, u32 remote_time)
{
	if (!m_keys.empty())
	{
		// the sender's clock went back a lot: its game was restarted or reloaded
		if (remote_time + 5000 < m_keys.back().t)
			reset();
		else if (remote_time <= m_keys.back().t)
			return; // old or repeated
		else
		{
			float gap = float(remote_time - m_keys.back().t);
			if (gap < 1000.f)
				m_interval = m_interval * 0.9f + gap * 0.1f;
			if (m_keys.back().p.distance_to(p) > teleport_distance)
			{
				m_keys.clear();
				m_last_valid = false;
			}
		}
	}

	// the fastest delivery seen is the best guess of the clock difference; creep up slowly
	// so a single early packet or a changed route does not hold the estimate forever
	s64 offset = s64(Device.dwTimeGlobal) - s64(remote_time);
	if (!m_offset_valid || offset < m_offset)
	{
		m_offset = offset;
		m_offset_valid = true;
	}
	else
		++m_offset;

	// how late this key is compared to the fastest one: keep the peak, let it fade over a few seconds
	float late = float(offset - m_offset);
	m_late = std::max(late, m_late * 0.97f);

	key k;
	k.t = remote_time;
	k.p = p;
	m_keys.push_back(k);
	while (m_keys.size() > max_keys)
		m_keys.pop_front();
}

bool CCoopNetPuppet::sample(Fvector& out)
{
	if (m_keys.empty())
		return false;

	u32 now = Device.dwTimeGlobal;

	// enough delay that the next key is almost always there before it is needed
	float target = m_fixed_delay ? float(m_fixed_delay) : clampr(m_late + m_interval + 20.f, min_delay, max_delay);
	if (m_last_valid && now > m_last_time)
	{
		float step = float(now - m_last_time) * delay_rate;
		m_delay += clampr(target - m_delay, -step, step);
	}

	s64 t = s64(now) - m_offset - s64(m_delay); // in the sender's clock

	while (m_keys.size() > 2 && s64(m_keys[1].t) <= t)
		m_keys.pop_front();

	const key& a = m_keys[0];
	if (m_keys.size() == 1 || t <= s64(a.t))
		out = a.p;
	else
	{
		const key& b = m_keys[1];
		float span = float(b.t - a.t);
		if (t <= s64(b.t))
			out.lerp(a.p, b.p, float(t - s64(a.t)) / span);
		else
		{
			// past the newest key: keep going along the last step for a moment, then wait
			float over = float(std::min<s64>(t - s64(b.t), max_extrapolation));
			Fvector v;
			v.sub(b.p, a.p).div(span);
			out.mad(b.p, v, over);
		}
	}

	if (m_last_valid && now > m_last_time)
	{
		Fvector d;
		d.sub(out, m_last);
		d.y = 0.f;
		float v = d.magnitude() / (float(now - m_last_time) / 1000.f);
		m_speed = m_speed * 0.8f + v * 0.2f;
	}
	m_last = out;
	m_last_time = now;
	m_last_valid = true;
	return true;
}
