// PS5SX2 (vk-285-133, AI-assisted): a check of frame generation's pacing (orbis-shims/OrbisFrameGenPacing.h) on a PC.
//   g++ -std=c++17 -Wall -Wextra -I../../orbis-shims -o /tmp/test_fg_pacing test_fg_pacing.cpp && /tmp/test_fg_pacing
// Copyright (C) 2026 Spyros
// SPDX-License-Identifier: GPL-3.0-or-later
#include "OrbisFrameGenPacing.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using orbis_fg::Decision;
using orbis_fg::Event;
using orbis_fg::Pacing;

static int s_failures = 0;
static int s_checks = 0;
#define CHECK(cond)                                                    \
	do                                                                 \
	{                                                                  \
		s_checks++;                                                    \
		if (!(cond))                                                   \
		{                                                              \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
			s_failures++;                                              \
		}                                                              \
	} while (0)

constexpr double kNtsc = 59.94, kPal = 50.0, kHz60 = 59.94, kHz120 = 119.88;

// A run of frames: each `vsyncs` apart on the PS2's clock, the emulation at `speed` percent (real time stretches to match).
struct Run
{
	Pacing p;
	double now = 100.0;
	double refreshes = 0.0; // the display's refreshes so far
	double presents = 0.0;  // the presents so far: the game's frames and the generated ones
	uint32_t frames = 0, engaged = 0, generated = 0, resets = 0;
	std::vector<Event> events;

	Decision Frame(uint32_t vsyncs, double ps2, double hz, double speed = 100.0, bool nominal = true)
	{
		const double clock = ps2 > 0.0 ? ps2 : 59.94, display = hz > 0.0 ? hz : 59.94; // what the pacing assumes then
		now += vsyncs / clock * (100.0 / speed);
		const Decision d = p.Frame(vsyncs, ps2, hz, now, speed, nominal);
		refreshes += vsyncs * display / clock;
		presents += 1 + d.presents;
		frames++;
		engaged += d.engaged ? 1 : 0;
		generated += d.presents;
		resets += d.reset ? 1 : 0;
		if (const Event e = p.TakeEvent(); e != Event::None)
			events.push_back(e);
		return d;
	}
};

static bool Has(const std::vector<Event>& events, Event e)
{
	for (Event x : events)
		if (x == e)
			return true;
	return false;
}

// A steady game: every frame the same, after a few to settle.
static Decision Steady(uint32_t vsyncs, double ps2, double hz, uint32_t frames = 50)
{
	Run r;
	Decision d;
	for (uint32_t i = 0; i < frames; i++)
		d = r.Frame(vsyncs, ps2, hz);
	CHECK(r.presents <= r.refreshes + 2.0); // never more presents than the display shows (two frames' slack at the start)
	return d;
}

int main()
{
	// The cases of the header's comment.
	{
		const Decision d = Steady(1, kNtsc, kHz60); // R&C at 60 Hz: what vk-285-131 slowed to half speed
		CHECK(!d.engaged && d.presents == 0);
	}
	{
		const Decision d = Steady(1, kNtsc, kHz120); // 60 fps on a 120 Hz display
		CHECK(d.engaged && d.presents == 1);
	}
	{
		const Decision d = Steady(2, kNtsc, kHz60); // 30 fps at 60 Hz
		CHECK(d.engaged && d.presents == 1);
	}
	{
		const Decision d = Steady(2, kNtsc, kHz120); // 30 fps at 120 Hz: the generated frame twice
		CHECK(d.engaged && d.presents == 2);
	}
	{
		const Decision d = Steady(3, kNtsc, kHz120); // 20 fps at 120 Hz: three times
		CHECK(d.engaged && d.presents == 3);
	}
	{
		const Decision d = Steady(3, kNtsc, kHz60); // 20 fps at 60 Hz: once, the game's frame up for two refreshes
		CHECK(d.engaged && d.presents == 1);
	}
	{
		const Decision d = Steady(4, kNtsc, kHz60); // 15 fps at 60 Hz
		CHECK(d.engaged && d.presents == 2);
	}
	{
		const Decision d = Steady(1, kPal, kHz120); // PAL 50 fps on 120 Hz: 2.4 refreshes a frame
		CHECK(d.engaged && d.presents == 1);
	}
	{
		const Decision d = Steady(2, kPal, kHz60); // PAL 25 fps at 60 Hz
		CHECK(d.engaged && d.presents == 1);
	}
	{
		const Decision d = Steady(1, kPal, kHz60); // PAL 50 fps at 60 Hz: 1.2 refreshes, no room
		CHECK(!d.engaged && d.presents == 0);
	}
	{
		const Decision d = Steady(1, 60.0, kHz120); // an HD mode's 60.00 Hz on 119.88: 1.998 refreshes
		CHECK(d.engaged && d.presents == 1);
	}
	{
		const Decision d = Steady(1, 59.83, kHz60); // NTSC progressive (59.83) at 59.94: still one refresh
		CHECK(!d.engaged && d.presents == 0);
	}
	{
		const Decision d = Steady(1, 0.0, 0.0); // rates unknown: 59.94 both
		CHECK(!d.engaged && d.presents == 0);
	}

	// The first engaged frame starts the interpolator over, the next ones don't.
	{
		Run r;
		Decision d = r.Frame(1, kNtsc, kHz120);
		CHECK(d.engaged && d.reset);
		d = r.Frame(1, kNtsc, kHz120);
		CHECK(d.engaged && !d.reset);
		CHECK(Has(r.events, Event::Engaged));
	}

	// A gap (a load): no frame that time, the next one starts over.
	{
		Run r;
		for (int i = 0; i < 20; i++)
			r.Frame(1, kNtsc, kHz120);
		Decision d = r.Frame(30, kNtsc, kHz120);
		CHECK(!d.engaged && d.presents == 0);
		d = r.Frame(1, kNtsc, kHz120);
		CHECK(d.engaged && d.reset && d.presents == 1);
	}

	// 60 fps games that drop a frame now and then, at 60 Hz: never a generated frame (vk-285-131's trap).
	{
		Run r;
		for (int i = 0; i < 600; i++)
			r.Frame((i % 7) == 3 ? 2 : 1, kNtsc, kHz60);
		CHECK(r.generated == 0);
		CHECK(r.presents <= r.refreshes + 0.5);
	}

	// A 30 fps game at 60 Hz with a short frame now and then: that frame gets no generated present (one refresh, no room).
	{
		Run r;
		uint32_t short_with_presents = 0;
		for (int i = 0; i < 600; i++)
		{
			const uint32_t v = (i % 9) == 4 ? 1 : 2;
			const Decision d = r.Frame(v, kNtsc, kHz60);
			if (v == 1 && d.presents > 0)
				short_with_presents++;
		}
		CHECK(short_with_presents == 0);
		CHECK(r.generated > 400);
		CHECK(r.presents <= r.refreshes + 2.0);
	}

	// Random frame patterns, both displays, both regions: never more presents than refreshes (two frames' slack).
	{
		srand(1234);
		for (int run = 0; run < 400; run++)
		{
			Run r;
			const double ps2 = (run & 1) ? kPal : kNtsc;
			const double hz = (run & 2) ? kHz120 : kHz60;
			const int base = 1 + rand() % 3;
			double max_queue = 0.0;
			for (int i = 0; i < 300; i++)
			{
				uint32_t v = static_cast<uint32_t>(base);
				const int roll = rand() % 10;
				if (roll == 0)
					v = 1;
				else if (roll == 1)
					v = static_cast<uint32_t>(base + 1);
				else if (roll == 2 && (rand() % 20) == 0)
					v = 12; // a load
				const double before = r.presents - r.refreshes;
				const Decision d = r.Frame(v, ps2, hz);
				// This frame's presents fit the refreshes its own interval had.
				CHECK(d.presents == 0 || d.presents + 1 <= static_cast<uint32_t>(v * hz / ps2 + 0.03));
				const double queue = before + 1 + d.presents; // presents waiting for refreshes right after this frame's
				if (queue > max_queue)
					max_queue = queue;
			}
			CHECK(r.presents <= r.refreshes + 2.0);
			CHECK(max_queue <= 4.0);
		}
	}

	// Turbo, slow motion, the fast boot: nothing generated, and back when the limiter is.
	{
		Run r;
		for (int i = 0; i < 20; i++)
			r.Frame(1, kNtsc, kHz120);
		r.events.clear();
		Decision d = r.Frame(1, kNtsc, kHz120, 300.0, false);
		CHECK(!d.engaged && d.presents == 0);
		CHECK(Has(r.events, Event::Disengaged));
		r.events.clear();
		d = r.Frame(1, kNtsc, kHz120);
		CHECK(d.engaged && d.reset && d.presents == 1);
		CHECK(Has(r.events, Event::Engaged));
	}

	// The safety net. Generated frames that cost speed: a 10 s pause, full speed in it, so the next pause is 20 s.
	{
		Run r;
		for (int i = 0; i < 120; i++) // two seconds at full speed
			r.Frame(1, kNtsc, kHz120);
		CHECK(!r.p.Paused());
		int slow_frames = 0;
		while (!r.p.Paused() && slow_frames < 1000)
		{
			r.Frame(1, kNtsc, kHz120, 80.0);
			slow_frames++;
		}
		CHECK(r.p.Paused());
		CHECK(Has(r.events, Event::Paused));
		CHECK(std::fabs(r.p.PauseSeconds() - 10.0) < 1e-9);
		CHECK(std::fabs(r.p.SlowSpeed() - 80.0) < 1e-9);
		CHECK(std::fabs(r.p.PauseFloor() - 95.0) < 1e-9);
		CHECK(slow_frames >= 48 * 2 && slow_frames <= 48 * 4); // two to four seconds at 80% (48 frames a second)
		// The pause: nothing generated, the game back at 100%.
		const double pause_start = r.now;
		uint32_t generated_in_pause = 0;
		while (r.p.Paused() && r.now - pause_start < 30.0)
		{
			const Decision in_pause = r.Frame(1, kNtsc, kHz120);
			if (r.p.Paused())
				generated_in_pause += in_pause.presents;
		}
		CHECK(generated_in_pause == 0);
		CHECK(r.now - pause_start >= 10.0 && r.now - pause_start < 11.5);
		CHECK(Has(r.events, Event::ResumedCostly));
		CHECK(r.p.NoFgSpeed() > 99.0);
		Decision d = r.Frame(1, kNtsc, kHz120);
		CHECK(d.engaged && d.presents == 1);
		// Slow again: the next pause is twice as long.
		r.events.clear();
		while (!r.p.Paused() && slow_frames < 5000)
		{
			r.Frame(1, kNtsc, kHz120, 80.0);
			slow_frames++;
		}
		CHECK(std::fabs(r.p.PauseSeconds() - 20.0) < 1e-9);
	}

	// A game slow by itself: as slow in the pause, so frames are generated again and its speed (less 3) is the floor.
	{
		Run r;
		for (int i = 0; i < 120; i++)
			r.Frame(1, kNtsc, kHz120);
		while (!r.p.Paused() && r.frames < 2000)
			r.Frame(1, kNtsc, kHz120, 85.0);
		CHECK(r.p.Paused());
		while (r.p.Paused() && r.frames < 4000)
			r.Frame(1, kNtsc, kHz120, 86.0);
		CHECK(Has(r.events, Event::ResumedSlowGame));
		CHECK(std::fabs(r.p.Floor(r.now) - 83.0) < 1e-9);
		// The same speed for 30 s more: no pause.
		const double until = r.now + 30.0;
		while (r.now < until)
			r.Frame(1, kNtsc, kHz120, 85.0);
		CHECK(!r.p.Paused());
		// Slower than that floor with the frames: a pause again, of 10 s.
		while (!r.p.Paused() && r.frames < 20000)
			r.Frame(1, kNtsc, kHz120, 75.0);
		CHECK(r.p.Paused());
		CHECK(std::fabs(r.p.PauseSeconds() - 10.0) < 1e-9);
		CHECK(std::fabs(r.p.PauseFloor() - 83.0) < 1e-9);
	}

	// A slow game with nothing generated (60 fps at 60 Hz): no pause, nothing to blame.
	{
		Run r;
		for (int i = 0; i < 2000; i++)
			r.Frame(1, kNtsc, kHz60, 50.0);
		CHECK(!r.p.Paused());
		CHECK(r.events.empty());
	}

	// The pause's length tops out at 160 s.
	{
		Run r;
		for (int round = 0; round < 7; round++)
		{
			while (!r.p.Paused() && r.frames < 200000)
				r.Frame(1, kNtsc, kHz120, 70.0);
			while (r.p.Paused() && r.frames < 400000)
				r.Frame(1, kNtsc, kHz120, 100.0);
		}
		while (!r.p.Paused() && r.frames < 600000)
			r.Frame(1, kNtsc, kHz120, 70.0);
		CHECK(std::fabs(r.p.PauseSeconds() - 160.0) < 1e-9);
	}

	printf("%s: %d of %d checks passed (frame generation's pacing)\n", s_failures ? "FAIL" : "PASS", s_checks - s_failures, s_checks);
	return s_failures ? 1 : 0;
}
