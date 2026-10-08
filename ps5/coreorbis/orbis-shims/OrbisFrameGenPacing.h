// PS5SX2 (vk-285-133, AI-assisted): frame generation's pacing, counted in the PS2's own vsyncs.
// Copyright (C) 2026 Spyros
// SPDX-License-Identifier: GPL-3.0-or-later
//
// vk-285-131 measured the game's frame interval on the wall clock, between presents. At 60 Hz that fed itself: once a
// generated frame had been shown (a slow first second was enough), it took a refresh of its own, so the game's frames came
// every two refreshes, which looked like a 30 fps game that should get a generated frame, and so on. Ratchet & Clank ran at
// 30 fps and half speed, with its GS and VU threads waiting on the display (2026-10-08, on the console).
//
// Here the interval is the number of PS2 vsyncs between two new frames of the game, which the presents can't change, and a
// frame's presents never exceed the display refreshes the PS2 gives it:
//     refreshes = vsyncs x display Hz / PS2 Hz
// The game's frame is one present. Frames are generated only when the game's frames have two refreshes or more (a 60 fps game
// on a 120 Hz display, a 30 fps one at 60 Hz), and the generated frame is presented for about half of them (once at 2, twice
// at 4, three times at 6); the game's frame stays up until the next one. A 60 fps game at 60 Hz gets none. While frames are
// generated, the vsyncs that only repeat the game's last frame present nothing (GSRenderer.cpp): the generated frame used
// their refresh.
//
// The safety net: when the game runs below 95% for 3 seconds while frames are generated, none are for 10 s. If the game is
// back at full speed in that pause, they were costing it, and the next pause is twice as long (up to 160 s). If it is as slow
// without them, the game is slow by itself there: frames are generated again, and for a minute that speed (less 3 points)
// is the floor. Nothing is generated while the frame limiter isn't at normal speed (turbo, slow motion, the fast boot).
// Header-only, so a PC test runs it (ps5/coreorbis/tests/fgpacing).

#pragma once

#include <cstdint>

namespace orbis_fg
{
	struct Decision
	{
		bool engaged = false;  // frames are generated for the game now: the interpolator takes every new frame
		bool reset = false;    // the interpolator's last frame is no neighbour of this one (a gap, a pause): it starts over
		uint32_t presents = 0; // presents of the generated frame before the game's, for this frame (0: none this time)
	};

	// What changed, for the log.
	enum class Event
	{
		None,
		Engaged,         // frames are generated: the game's frames have room for another
		Disengaged,      // no room any more, or the limiter left normal speed
		Paused,          // the game slowed while frames were generated: none for PauseSeconds()
		ResumedCostly,   // the game was at full speed without them: generated again, the next pause longer
		ResumedSlowGame, // it was as slow without them: generated again, with a lower floor for a minute
	};

	class Pacing
	{
	public:
		static constexpr uint32_t kGapVsyncs = 8;      // a longer wait between two frames: a load, a pause, a still picture
		static constexpr double kEngage = 1.97;        // two refreshes a frame, less rounding (an HD mode's 60 Hz on 119.88: 1.998)
		static constexpr double kDisengage = 1.6;      // ...kept down to here (a 30 fps game's odd short frame), each frame still capped
		static constexpr double kSlowSpeed = 95.0;     // percent
		static constexpr int kSlowSeconds = 3;
		static constexpr double kFirstPause = 10.0;    // seconds
		static constexpr double kMaxPause = 160.0;
		static constexpr double kSettle = 2.0;         // a pause's first seconds aren't the game's speed without them yet
		static constexpr double kFloorSeconds = 60.0;
		static constexpr double kCostly = 3.0;         // points of speed a pause has to win back to blame the generated frames
		static constexpr double kMeanWeight = 0.1;     // a frame's weight in the mean

		// One new frame of the game: `vsyncs` since the last new frame (1 at 60 fps, 2 at 30), the PS2's and the display's rates
		// in Hz, `now` in seconds (any steady clock), the emulation's speed in percent, and whether the limiter is at normal speed.
		Decision Frame(uint32_t vsyncs, double ps2_hz, double display_hz, double now, double speed, bool nominal)
		{
			Decision d;
			if (vsyncs == 0)
				vsyncs = 1;
			if (!(ps2_hz >= 20.0 && ps2_hz <= 100.0))
				ps2_hz = 59.94;
			if (!(display_hz >= 20.0 && display_hz <= 250.0))
				display_hz = 59.94;
			const double ratio = display_hz / ps2_hz;

			// A long wait: the mean starts again, and so does the interpolator.
			const bool gap = vsyncs > kGapVsyncs;
			if (gap)
			{
				m_mean = 0.0;
				m_reset = true;
			}
			else
			{
				m_mean = m_mean > 0.0 ? m_mean * (1.0 - kMeanWeight) + vsyncs * kMeanWeight : vsyncs;
			}
			m_refreshes = m_mean * ratio;
			const double now_refreshes = vsyncs * ratio;

			// The generated frame's presents: about half the refreshes a frame has, kept from flickering between two counts.
			const double half = m_refreshes / 2.0;
			while (m_repeats < 3 && half >= m_repeats + 0.9)
				m_repeats++;
			while (m_repeats > 1 && half < m_repeats - 0.2)
				m_repeats--;

			Second(now, speed);

			const bool room = !gap && m_mean > 0.0 && m_refreshes >= (m_room ? kDisengage : kEngage);
			if (room != m_room)
			{
				m_room = room;
				if (!Paused() && nominal)
					Raise(room ? Event::Engaged : Event::Disengaged);
			}
			if (nominal != m_nominal)
			{
				m_nominal = nominal;
				if (!Paused() && m_room)
					Raise(nominal ? Event::Engaged : Event::Disengaged);
			}

			d.engaged = m_room && nominal && !Paused();
			if (!d.engaged)
			{
				m_reset = true; // the frames skipped meanwhile weren't given to the interpolator
				m_engaged = false;
				return d;
			}
			if (!m_engaged)
			{
				m_engaged = true;
				m_reset = true;
			}

			// This frame's presents: never more than the refreshes its own interval had, less the game's frame.
			const uint32_t fit = static_cast<uint32_t>(now_refreshes + 0.03);
			d.presents = fit > 1 ? (m_repeats < fit - 1 ? m_repeats : fit - 1) : 0;
			d.reset = m_reset;
			m_reset = false;
			if (d.presents > 0)
				m_generated_this_second = true;
			return d;
		}

		Event TakeEvent()
		{
			const Event e = m_event;
			m_event = Event::None;
			return e;
		}

		double MeanVsyncs() const { return m_mean; }
		double Refreshes() const { return m_refreshes; }
		uint32_t Repeats() const { return m_repeats; }
		bool Paused() const { return m_paused_until > 0.0; }
		double PauseSeconds() const { return m_pause_length; }
		double SlowSpeed() const { return m_fg_speed; }     // the speed that started the last pause
		double PauseFloor() const { return m_pause_floor; } // the floor it fell below
		double NoFgSpeed() const { return m_nofg_speed; }   // the speed during the last pause
		double Floor(double now) const { return now < m_floor_until ? m_floor : kSlowSpeed; }

	private:
		// A room change doesn't hide a pause or a resume not yet logged.
		void Raise(Event e)
		{
			if (m_event == Event::None || m_event == Event::Engaged || m_event == Event::Disengaged)
				m_event = e;
		}

		// Once a second: the speed against the floor while frames are generated, and the pauses.
		void Second(double now, double speed)
		{
			if (!m_second_started)
			{
				m_second_started = true;
				m_second = now;
				return;
			}
			if (now - m_second < 1.0)
				return;
			m_second = now;
			const bool generated = m_generated_this_second;
			m_generated_this_second = false;

			if (Paused())
			{
				if (now >= m_pause_start + kSettle)
				{
					m_nofg_sum += speed;
					m_nofg_n++;
				}
				if (now < m_paused_until)
					return;
				m_paused_until = 0.0;
				m_slow = 0;
				m_slow_sum = 0.0;
				m_nofg_speed = m_nofg_n > 0 ? m_nofg_sum / m_nofg_n : speed;
				m_nofg_sum = 0.0;
				m_nofg_n = 0;
				if (m_nofg_speed >= m_fg_speed + kCostly)
				{
					m_next_pause = m_next_pause * 2.0 < kMaxPause ? m_next_pause * 2.0 : kMaxPause;
					m_event = Event::ResumedCostly;
				}
				else
				{
					m_floor = m_nofg_speed - kCostly < kSlowSpeed ? m_nofg_speed - kCostly : kSlowSpeed;
					m_floor_until = now + kFloorSeconds;
					m_next_pause = kFirstPause;
					m_event = Event::ResumedSlowGame;
				}
				m_reset = true;
				return;
			}

			if (generated && speed < Floor(now))
			{
				m_slow++;
				m_slow_sum += speed;
			}
			else
			{
				m_slow = 0;
				m_slow_sum = 0.0;
			}
			if (m_slow >= kSlowSeconds)
			{
				m_fg_speed = m_slow_sum / m_slow;
				m_pause_floor = Floor(now);
				m_pause_length = m_next_pause;
				m_pause_start = now;
				m_paused_until = now + m_pause_length;
				m_slow = 0;
				m_slow_sum = 0.0;
				m_event = Event::Paused;
			}
		}

		double m_mean = 0.0;
		double m_refreshes = 0.0;
		uint32_t m_repeats = 1;
		bool m_room = false;
		bool m_nominal = true;
		bool m_engaged = false;
		bool m_reset = true;
		bool m_generated_this_second = false;
		Event m_event = Event::None;

		bool m_second_started = false;
		double m_second = 0.0;
		int m_slow = 0;
		double m_slow_sum = 0.0;
		double m_fg_speed = 0.0;
		double m_pause_floor = kSlowSpeed;
		double m_pause_start = 0.0;
		double m_paused_until = 0.0;
		double m_pause_length = kFirstPause;
		double m_next_pause = kFirstPause;
		double m_nofg_sum = 0.0;
		int m_nofg_n = 0;
		double m_nofg_speed = 0.0;
		double m_floor = kSlowSpeed;
		double m_floor_until = 0.0;
	};
} // namespace orbis_fg
