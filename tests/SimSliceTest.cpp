/*
 *  This file is part of sc4-cpu-options.
 *  Copyright (C) 2026 Casper Van Gheluwe
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

// Build as a 32-bit console program with src/SimSlice.cpp, src/GameVersion.cpp and src/Logger.cpp;
// it returns 0 when every check passes. Asserts are on in Debug builds only.

#include "SimSlice.h"

#include <cassert>

using namespace SimSlice;

int main()
{
	// -SimEarlyExit: milliseconds 0..100, off, or the default.
	assert(ParseEarlyExit(nullptr) == kDefaultEarlyExitMs);
	assert(ParseEarlyExit("") == kDefaultEarlyExitMs);
	assert(ParseEarlyExit("junk") == kDefaultEarlyExitMs);
	assert(ParseEarlyExit("0") == 0);
	assert(ParseEarlyExit("10") == 10);
	assert(ParseEarlyExit("500") == 100);
	assert(ParseEarlyExit("-3") < 0);
	assert(ParseEarlyExit("off") < 0);
	assert(ParseEarlyExit("OFF") < 0);
	assert(ParseEarlyExit("false") < 0);

	// -SimDayWait: auto (a bare switch), off, or milliseconds 0..100. Not given at all: off.
	assert(ParseDayWait(nullptr) == kDayWaitAuto);   // a bare -SimDayWait
	assert(ParseDayWait("auto") == kDayWaitAuto);
	assert(ParseDayWait("Auto") == kDayWaitAuto);
	assert(ParseDayWait("junk") == kDayWaitAuto);
	assert(ParseDayWait("off") == kDayWaitOff);
	assert(ParseDayWait("-1") == kDayWaitOff);
	assert(ParseDayWait("0") == 0);
	assert(ParseDayWait("8") == 8);
	assert(ParseDayWait("1000") == 100);

	// -SimFrameCap: off/0 for uncapped, else fps clamped to 10..1000.
	assert(ParseFrameCap(nullptr) == 0);
	assert(ParseFrameCap("off") == 0);
	assert(ParseFrameCap("0") == 0);
	assert(ParseFrameCap("-60") == 0);
	assert(ParseFrameCap("junk") == 0);
	assert(ParseFrameCap("60") == 60);
	assert(ParseFrameCap("5") == 10);
	assert(ParseFrameCap("5000") == 1000);

	// Defaults: early exit on with a 2 ms allowance, no day wait, no frame cap, no probe.
	Options const defaults;
	assert(defaults.earlyExitMs == 2);
	assert(defaults.dayWaitMs == kDayWaitOff);
	assert(defaults.frameCapFps == 0);
	assert(!defaults.probe);

	return 0;
}
