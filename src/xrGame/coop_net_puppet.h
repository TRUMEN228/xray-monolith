////////////////////////////////////////////////////////////////////////////
//	Module 		: coop_net_puppet.h
//	Description : Anomaly Co-op: a monster whose position comes from the network
//	              (the partner's puppet) instead of its own path
////////////////////////////////////////////////////////////////////////////

#pragma once

// Positions arrive with the sender's clock (its time_global). They are played back a little
// behind real time, so uneven delivery on a laggy link is absorbed, and interpolated every frame.
class CCoopNetPuppet
{
public:
	struct key
	{
		u32 t; // sender's time, ms
		Fvector p;
	};

	CCoopNetPuppet(u32 delay);

	void push(const Fvector& p, u32 remote_time);
	// position for this frame; false while nothing has arrived yet
	bool sample(Fvector& out);
	float speed() const { return m_speed; }
	void set_delay(u32 delay) { m_delay = delay; }

private:
	void reset();

	xr_deque<key> m_keys;
	u32 m_delay; // playback delay, ms
	s64 m_offset; // local time minus sender's time, the smallest seen lately
	bool m_offset_valid;
	Fvector m_last;
	u32 m_last_time;
	bool m_last_valid;
	float m_speed; // horizontal, m/s, smoothed
};
