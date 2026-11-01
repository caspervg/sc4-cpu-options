/*
 *  This file is part of sc4-cpu-options.
 *  Copyright (C) 2026 Casper Van Gheluwe
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

namespace SimSlice
{

	// The idle-time allowance used when the command line does not set one.
	constexpr int kDefaultEarlyExitMs = 2;

	struct Options
	{
		// Milliseconds of idle-time agent work to allow after the simulator has
		// caught up, before leaving the slice early. Negative turns the early
		// exit off (stock behavior).
		int earlyExitMs = kDefaultEarlyExitMs;

		// Frames shorter than 1000/frameCapFps ms sleep the rest of the frame
		// once the simulator has caught up, on a high-resolution waitable timer.
		// The sleep ends early for a sim day that falls due, so the cap does not
		// make days start late. 0 leaves the frame rate uncapped. Needs the
		// early exit.
		int frameCapFps = 0;

		// Log slice, fps and sim-day statistics every 5 seconds.
		bool probe = false;
	};

	// Parses an -SimEarlyExit value: "off" (or "no", "false"), or milliseconds
	// (0 to 100). An empty or unreadable value keeps the default.
	int ParseEarlyExit(const char* value);

	// Parses an -SimFrameCap value: "off" or 0 for uncapped, else frames per
	// second (10 to 1000).
	int ParseFrameCap(const char* value);

	// cSC4Simulator::OnTick runs the simulation for a fixed slice every frame
	// (1000 / 30, 20 or 15 ms, by speed), however little work there is. The sim
	// clock is gated by wall-clock time, so once the agents have caught up the
	// rest of the slice only spins, which caps the frame rate at 30/20/15 fps.
	// The early exit leaves the slice once the simulator has caught up and the
	// idle-time agents have had their allowance, so frames are only as long as
	// the real work, and sim days per second stay as in the stock game. A
	// simulator that cannot keep up never catches up, and then the slice runs
	// exactly as in the stock game.
	//
	// Requires SimCity 4 Deluxe 1.1.641. Safe to call twice; only the first
	// installs. Returns true if a patch is active afterwards.
	bool Install(const Options& options);
	void Uninstall(void);

}
