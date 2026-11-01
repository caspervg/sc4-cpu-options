/*
 *  This file is part of sc4-cpu-options.
 *  Copyright (C) 2026 Casper Van Gheluwe
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

// Leave cSC4Simulator::OnTick's per-tick slice early once the sim has caught up.
//
// OnTick (VA 0x00703E70, ESI = this) runs
//   do { if (!paused) { SimulateTimePassage; SimulateAgents; }   // 0x00702A80, 0x007038B0
//        SimulateIdleTime; }                                     // 0x00702DF0
//   while (cRZTimer::GetElapsedTime(this+0x190) < [this+0x70]);
// The budget [this+0x70] is 1000/divisor - (time outside the slice), divisor
// 30/20/15 by speed (DoSetSimSpeed_ 0x00702430), floored at 15 ms. A sim day
// only starts when the previous day's agents are done and the speed's
// wall-clock interval ("Simulator Speeds" exemplar property 0x89590E6D) has
// passed. SimulateAgents and SimulateIdleTime share the slice timer, so once
// the agent tiers are exhausted the rest of the slice is spent on further
// SimulateIdleTime rounds.
//
// SimulateIdleTime sends message 0x6695681A to each enabled idle-time agent in
// turn, with a time slice of min(budget left, budget / 3) and the paused state
// (which the handlers read so far ignore), until one full round is done or the
// budget runs out. In 1.1.641 the idle agents are the growth developers (one per
// occupant group), building development, demand, land value, aura, crime,
// commercial, power, plumbing, prop developer, neighbors and residential. Most
// run a staged cycle started by their monthly simulation (growth planning, the
// demand census) and return at once when it is done; neighbors only pushes deal
// changes to the region; residential ignores the message. So after a cycle
// completes, the extra rounds mostly find nothing to do.
//
// The early exit is checked only at the end of a pass, so every slice gives each
// idle agent at least one turn, and the allowance after catching up is a
// minimum: the pass that crosses it runs to its end. Paused frames count as
// caught up at once (the sim clock is stopped, so no new cycles start), and so
// get one round plus the allowance; the stock game spends the whole slice.
//
// Two patches, both 5-byte jumps to naked stubs:
//   0x00703F18  8D 9E 90 01 00 00     lea ebx, [esi+0x190]            loop entry -> OnTickBegin
//   0x00703F53  8B CB E8 3E C2 18 00  mov ecx, ebx; call Elapsed       loop tail  -> OnPass
// The tail stub leaves the loop at 0x00703F5F when OnPass says so, or zeroes the
// elapsed time in EAX to force one more pass past the budget (EAX is reloaded at
// the loop top, 0x00703F25).
//
// Frame cap: once caught up, the tail sleeps until the frame's end, less the
// measured time outside the slice, on a high-resolution waitable timer (a
// normal one plus timeBeginPeriod(1) before Windows 10 1803). A sim day that
// falls due first ends the sleep, and a forced pass starts it at once: the
// day timer restarts at each step, so a late day would be lost for good. Loop
// entry tops the sleep up to the full frame, which also caps frames whose
// slice ends at the hidden pause (scrolling) without reaching the tail.
//
// Simulator fields, relative to OnTick's ESI (the agent methods see ESI-8):
//   +0x028  int   paused count            +0x034  int  speed index (0..2)
//   +0x040  cRZTimer  time since the last gated clock step (restarted by each step)
//   +0x070  uint  slice budget (ms)       +0x078  uint gated-step interval (ms per sim day)
//   +0x0D4  u8[8] step flags, cleared and set by SimulateTimePassage each pass
//                 (+0x0D8 = a new sim day started)
//   +0x10C + 12*i  agent tier i end (tree header node), i = 0..9
//   +0x110 + 12*i  agent tier i size
//   +0x1A8 + 4*i   agent tier i cursor (node), equals end when the tier is done
//   +0x184 / +0x188 / +0x1D0  idle-time list header / size / cursor
// Tiers 0/1 are re-run whenever exhausted; they were empty while running in
// measured cities, so "caught up" only looks at tiers 2..9. Trees are STLport
// rb-trees: node { color, parent +4, left +8, right +0xC, agent* +0x10,
// enabled u8 +0x14 }, header->left = leftmost.
//
// Target: SimCity 4 Deluxe 1.1.641 (Windows), image base 0x00400000.

#include "SimSlice.h"

#include "GameVersion.h"
#include "Logger.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <Windows.h>
#include <mmsystem.h>

#pragma comment(lib, "winmm.lib")

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace SimSlice
{
	namespace
	{

		constexpr uintptr_t kImageBase = 0x00400000;

		constexpr uintptr_t kEntryVA = 0x00703F18;
		constexpr uintptr_t kEntryRejoinVA = 0x00703F1E;
		constexpr size_t kEntryLen = 6;
		const uint8_t kEntryExpected[kEntryLen] = { 0x8D, 0x9E, 0x90, 0x01, 0x00, 0x00 };

		constexpr uintptr_t kTailVA = 0x00703F53;
		constexpr uintptr_t kTailRejoinVA = 0x00703F5A;   // cmp eax, [esi+0x70]; jc loop
		constexpr uintptr_t kLoopExitVA = 0x00703F5F;     // first instruction after the loop
		constexpr size_t kTailLen = 7;
		const uint8_t kTailExpected[kTailLen] = { 0x8B, 0xCB, 0xE8, 0x3E, 0xC2, 0x18, 0x00 };

		constexpr uintptr_t kGetElapsedVA = 0x00890198;   // cRZTimer::GetElapsedTime

		constexpr size_t kTierCount = 10;
		constexpr size_t kStepFlagCount = 8;
		constexpr int kMaxEarlyExitMs = 100;
		constexpr int kMinFrameCapFps = 10;
		constexpr int kMaxFrameCapFps = 1000;
		constexpr DWORD kWindowMs = 5000;

		// The time between slices, smoothed, ignoring gaps from pauses and loading.
		constexpr double kOutsideSmoothing = 0.2;
		constexpr double kMaxOutsideSampleMs = 250.0;

		// The timer wakes this early and the rest is spun, to hide its jitter.
		constexpr double kSpinMarginHighResMs = 0.5;
		constexpr double kSpinMarginLegacyMs = 1.0;

		// OnPass results, read by the tail stub (which repeats the values as literals).
		constexpr int kContinue = 0;      // the loop's own budget check decides
		constexpr int kLeave = 1;         // leave the slice now
		constexpr int kAnotherPass = 2;   // run one more pass, even past the budget
		static_assert(kLeave == 1 && kAnotherPass == 2, "TailStub compares against these literals");

		uintptr_t gEntryRejoin = 0;
		uintptr_t gTailRejoin = 0;
		uintptr_t gLoopExit = 0;
		uintptr_t gGetElapsed = 0;   // read by the tail stub

		using GetElapsedFn = uint32_t(__thiscall*)(const void* timer);
		GetElapsedFn gGetElapsedFn = nullptr;

		Options gOptions;

		// Time between slices, measured with the performance counter.
		double gQpcMsPerTick = 0.0;
		int64_t gLastPassQpc = 0;
		double gOutsideMs = 0.0;    // smoothed
		bool gHaveOutside = false;

		// Frame cap.
		int64_t gMinFrameTicks = 0;   // QPC ticks per capped frame; 0 when uncapped
		int64_t gTickBeginQpc = 0;    // when the current slice began
		HANDLE gTimer = nullptr;
		bool gTimerHighRes = false;
		bool gTimePeriodRaised = false;
		double gSpinMarginMs = 0.0;

		int64_t QpcNow()
		{
			LARGE_INTEGER now;
			QueryPerformanceCounter(&now);
			return now.QuadPart;
		}

		bool CreateFrameTimer()
		{
			gTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
			if (gTimer != nullptr)
			{
				gTimerHighRes = true;
				gSpinMarginMs = kSpinMarginHighResMs;
				return true;
			}
			// Before Windows 10 1803: a normal timer, at the 1 ms timer resolution.
			gTimePeriodRaised = timeBeginPeriod(1) == TIMERR_NOERROR;
			gTimer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
			gSpinMarginMs = kSpinMarginLegacyMs;
			return gTimer != nullptr;
		}

		void DestroyFrameTimer()
		{
			if (gTimer != nullptr) CloseHandle(gTimer);
			gTimer = nullptr;
			if (gTimePeriodRaised) timeEndPeriod(1);
			gTimePeriodRaised = false;
			gTimerHighRes = false;
		}

		// Sleeps until the performance counter reaches target. Returns the ms slept.
		double SleepUntil(int64_t target)
		{
			int64_t const start = QpcNow();
			if (target <= start) return 0.0;
			double const waitMs = static_cast<double>(target - start) * gQpcMsPerTick - gSpinMarginMs;
			if (waitMs >= 0.1)
			{
				bool waited = false;
				if (gTimer != nullptr)
				{
					LARGE_INTEGER due;
					due.QuadPart = -static_cast<LONGLONG>(waitMs * 10000.0);   // relative, 100 ns units
					if (SetWaitableTimer(gTimer, &due, 0, nullptr, nullptr, FALSE))
					{
						waited = WaitForSingleObject(gTimer, INFINITE) == WAIT_OBJECT_0;
					}
				}
				if (!waited && waitMs >= 1.0) Sleep(static_cast<DWORD>(waitMs));
			}
			while (QpcNow() < target) YieldProcessor();
			return static_cast<double>(QpcNow() - start) * gQpcMsPerTick;
		}

		struct Patch
		{
			uint8_t* target = nullptr;
			size_t length = 0;
			uint8_t saved[8]{};
		};
		Patch gEntryPatch;
		Patch gTailPatch;
		bool gInstalled = false;

		template <typename T>
		T Read(const uint8_t* base, size_t offset)
		{
			T value;
			std::memcpy(&value, base + offset, sizeof(T));
			return value;
		}

		uintptr_t TierEnd(const uint8_t* sim, size_t i) { return Read<uintptr_t>(sim, 0x10C + 12 * i); }
		uint32_t TierSize(const uint8_t* sim, size_t i) { return Read<uint32_t>(sim, 0x110 + 12 * i); }
		uintptr_t TierCursor(const uint8_t* sim, size_t i) { return Read<uintptr_t>(sim, 0x1A8 + 4 * i); }

		// ---- per-tick state, game thread only ----

		constexpr uint32_t kNotCaughtUp = 0xFFFFFFFF;

		struct TickState
		{
			bool active = false;
			bool paused = false;
			uint32_t passes = 0;
			uint32_t budget = 0;
			uint32_t lastElapsed = 0;
			uint32_t caughtUpAt = kNotCaughtUp;   // slice time the current caught-up run began
			uint32_t idleOnlyMs = 0;              // time in passes that began and ended caught up
			bool prevCaughtUp = false;
			bool exitedEarly = false;
			bool sleptForFrameCap = false;
			double sleptMs = 0.0;
			uint32_t dayWakes = 0;
		};

		TickState gTick;

		// ---- probe statistics ----

		struct Window
		{
			uint32_t ticks = 0;
			uint64_t passes = 0;
			uint64_t budget = 0;
			uint64_t slice = 0;
			uint64_t idleOnly = 0;
			uint32_t ticksWithIdleOnly = 0;
			uint32_t earlyExits = 0;
			uint32_t frameCapSleeps = 0;
			double sleptMs = 0.0;
			uint32_t dayWakes = 0;

			uint32_t pausedTicks = 0;
			uint64_t pausedSlice = 0;

			uint32_t allTicks = 0;      // every OnTick that reached the loop, any state
			uint32_t simDays = 0;
			int32_t speed = -1;
			bool mixedSpeed = false;
		};

		Window gWindow;
		DWORD gWindowStart = 0;

		void FinishTick()
		{
			if (!gTick.active) return;
			gWindow.allTicks++;
			gWindow.sleptMs += gTick.sleptMs;
			gWindow.dayWakes += gTick.dayWakes;
			if (gTick.sleptForFrameCap) gWindow.frameCapSleeps++;
			if (gTick.passes > 0)
			{
				if (gTick.paused)
				{
					gWindow.pausedTicks++;
					gWindow.pausedSlice += gTick.lastElapsed;
				}
				else
				{
					gWindow.ticks++;
					gWindow.passes += gTick.passes;
					gWindow.budget += gTick.budget;
					gWindow.slice += gTick.lastElapsed;
					gWindow.idleOnly += gTick.idleOnlyMs;
					if (gTick.idleOnlyMs > 0) gWindow.ticksWithIdleOnly++;
				}
				if (gTick.exitedEarly) gWindow.earlyExits++;
			}
			gTick = TickState{};
		}

		// ---- agent census, probe only ----

		struct AgentEntry
		{
			uintptr_t vtable;
			uint32_t count;
			uint32_t enabled;
		};

		constexpr size_t kMaxEntries = 24;

		struct Census
		{
			AgentEntry entries[kMaxEntries];
			size_t used;
			uint32_t nodes;
			bool faulted;
		};

		uintptr_t Successor(uintptr_t node)
		{
			uintptr_t right = *reinterpret_cast<uintptr_t*>(node + 0xC);
			if (right != 0)
			{
				node = right;
				while (*reinterpret_cast<uintptr_t*>(node + 0x8) != 0)
				{
					node = *reinterpret_cast<uintptr_t*>(node + 0x8);
				}
				return node;
			}
			uintptr_t parent = *reinterpret_cast<uintptr_t*>(node + 0x4);
			while (node == *reinterpret_cast<uintptr_t*>(parent + 0xC))
			{
				node = parent;
				parent = *reinterpret_cast<uintptr_t*>(node + 0x4);
			}
			if (*reinterpret_cast<uintptr_t*>(node + 0xC) != parent) node = parent;
			return node;
		}

		// No C++ objects with destructors in here, so SEH is allowed.
		void TakeCensus(uintptr_t header, uint32_t size, Census& out)
		{
			std::memset(&out, 0, sizeof(out));
			if (header == 0 || size == 0) return;
			__try
			{
				uintptr_t node = *reinterpret_cast<uintptr_t*>(header + 0x8);
				uint32_t guard = size + 1;
				while (node != header && guard-- > 0)
				{
					uintptr_t agent = *reinterpret_cast<uintptr_t*>(node + 0x10);
					bool enabled = *reinterpret_cast<uint8_t*>(node + 0x14) != 0;
					uintptr_t vtable = agent != 0 ? *reinterpret_cast<uintptr_t*>(agent) : 0;
					size_t i = 0;
					while (i < out.used && out.entries[i].vtable != vtable) i++;
					if (i == out.used && out.used < kMaxEntries)
					{
						out.entries[out.used++] = AgentEntry{ vtable, 0, 0 };
					}
					if (i < out.used)
					{
						out.entries[i].count++;
						if (enabled) out.entries[i].enabled++;
					}
					out.nodes++;
					node = Successor(node);
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				out.faulted = true;
			}
		}

		void LogCensus(const char* name, uintptr_t header, uint32_t size)
		{
			Census census;
			TakeCensus(header, size, census);
			char line[1024];
			int n = std::snprintf(line, sizeof(line), "sim slice: %s size=%u walked=%u%s:",
				name, size, census.nodes, census.faulted ? " FAULTED" : "");
			for (size_t i = 0; i < census.used && n > 0 && n < static_cast<int>(sizeof(line)); i++)
			{
				n += std::snprintf(line + n, sizeof(line) - n, " %08X x%u(on %u)",
					static_cast<unsigned>(census.entries[i].vtable), census.entries[i].count, census.entries[i].enabled);
			}
			Logger::GetInstance().WriteLine(LogLevel::Info, line);
		}

		const char* SpeedName(int32_t speed)
		{
			switch (speed)
			{
			case 0: return "turtle";
			case 1: return "llama";
			case 2: return "cheetah";
			default: return "?";
			}
		}

		void LogWindow(const uint8_t* sim, DWORD elapsedMs)
		{
			Logger& logger = Logger::GetInstance();
			Window const& w = gWindow;
			double const seconds = elapsedMs / 1000.0;

			logger.WriteLineFormatted(LogLevel::Info,
				"sim slice: fps=%.1f simDays/s=%.2f speed=%s%s earlyExits=%u frameCapSleeps=%u slept=%.1f ms/frame dayWakes=%u outside=%.1f ms",
				w.allTicks / seconds, w.simDays / seconds, SpeedName(w.speed), w.mixedSpeed ? "(mixed)" : "",
				w.earlyExits, w.frameCapSleeps, w.allTicks > 0 ? w.sleptMs / w.allTicks : 0.0, w.dayWakes, gOutsideMs);
			if (w.ticks > 0)
			{
				double const t = static_cast<double>(w.ticks);
				logger.WriteLineFormatted(LogLevel::Info,
					"sim slice: running ticks=%u passes/tick=%.1f budget=%.1f ms slice=%.1f ms idleOnly=%.1f ms ticksWithIdleOnly=%u%%",
					w.ticks, w.passes / t, w.budget / t, w.slice / t, w.idleOnly / t,
					static_cast<unsigned>(100.0 * w.ticksWithIdleOnly / t));
			}
			if (w.pausedTicks > 0)
			{
				logger.WriteLineFormatted(LogLevel::Info, "sim slice: paused ticks=%u slice=%.1f ms",
					w.pausedTicks, w.pausedSlice / static_cast<double>(w.pausedTicks));
			}
			if (sim != nullptr)
			{
				char line[256];
				int n = std::snprintf(line, sizeof(line), "sim slice: tier sizes");
				for (size_t i = 0; i < kTierCount && n > 0 && n < static_cast<int>(sizeof(line)); i++)
				{
					n += std::snprintf(line + n, sizeof(line) - n, " %u", TierSize(sim, i));
				}
				if (n > 0 && n < static_cast<int>(sizeof(line)))
				{
					std::snprintf(line + n, sizeof(line) - n, " idle=%u", Read<uint32_t>(sim, 0x188));
				}
				logger.WriteLine(LogLevel::Info, line);
				LogCensus("tier0", TierEnd(sim, 0), TierSize(sim, 0));
				LogCensus("idle", Read<uintptr_t>(sim, 0x184), Read<uint32_t>(sim, 0x188));
			}
			gWindow = Window{};
		}

		// ---- hooks ----

		// The time from the end of the previous slice to now: rendering and the
		// other tick services. Pauses, loading and hidden-paused stretches show
		// up as long gaps and are ignored.
		void MeasureOutside()
		{
			if (gLastPassQpc == 0) return;
			double const sample = static_cast<double>(QpcNow() - gLastPassQpc) * gQpcMsPerTick;
			gLastPassQpc = 0;
			if (sample < 0.0 || sample > kMaxOutsideSampleMs) return;
			gOutsideMs = gHaveOutside ? gOutsideMs + kOutsideSmoothing * (sample - gOutsideMs) : sample;
			gHaveOutside = true;
		}

		void __stdcall OnTickBegin(const uint8_t* sim)
		{
			FinishTick();
			MeasureOutside();
			if (gMinFrameTicks > 0)
			{
				// Top up to the full frame: the tail's sleep only estimates the time
				// outside the slice, and slices cut short by the hidden pause never
				// reach the tail.
				if (gTickBeginQpc != 0)
				{
					double const slept = SleepUntil(gTickBeginQpc + gMinFrameTicks);
					if (slept > 0.0)
					{
						gTick.sleptMs += slept;
						gTick.sleptForFrameCap = true;
					}
				}
				gTickBeginQpc = QpcNow();
			}
			if (gOptions.probe)
			{
				DWORD const now = GetTickCount();
				if (gWindowStart == 0) gWindowStart = now;
				if (now - gWindowStart >= kWindowMs)
				{
					LogWindow(sim, now - gWindowStart);
					gWindowStart = now;
				}
				int32_t const speed = Read<int32_t>(sim, 0x34);
				if (gWindow.speed == -1) gWindow.speed = speed;
				else if (gWindow.speed != speed) gWindow.mixedSpeed = true;
			}
			gTick.active = true;
			gTick.paused = Read<int32_t>(sim, 0x28) > 0;
			gTick.budget = Read<uint32_t>(sim, 0x70);
		}

		bool CaughtUp(const uint8_t* sim)
		{
			// Paused: only SimulateIdleTime runs, and with the clock stopped no new
			// idle cycles start, so the first round is enough to count as caught up.
			if (gTick.paused) return true;
			for (size_t i = 0; i < kStepFlagCount; i++)
			{
				if (sim[0xD4 + i] != 0) return false;   // the clock stepped this pass: new work queued
			}
			for (size_t i = 2; i < kTierCount; i++)
			{
				if (TierCursor(sim, i) != TierEnd(sim, i)) return false;
			}
			return true;
		}

		// The frame cap, once caught up: sleep until the frame's end, less the
		// expected time outside the slice, or until the next sim day if that is
		// sooner. The day timer restarts at each step, so a day that starts late
		// loses that time for good; a forced pass starts it at once instead.
		int SleepRestOfFrame(const uint8_t* sim)
		{
			int64_t const outside = gHaveOutside ? static_cast<int64_t>(gOutsideMs / gQpcMsPerTick) : 0;
			int64_t const frameEnd = gTickBeginQpc + gMinFrameTicks - outside;
			int64_t const now = QpcNow();
			if (now >= frameEnd) return kLeave;

			int64_t wake = frameEnd;
			bool forDay = false;
			if (!gTick.paused)
			{
				uint32_t const interval = Read<uint32_t>(sim, 0x78);
				uint32_t const sinceStep = gGetElapsedFn(sim + 0x40);
				if (sinceStep >= interval)
				{
					gTick.dayWakes++;
					return kAnotherPass;   // already due
				}
				int64_t const due = now + static_cast<int64_t>((interval - sinceStep) / gQpcMsPerTick);
				if (due < frameEnd)
				{
					wake = due;
					forDay = true;
				}
			}

			gTick.sleptMs += SleepUntil(wake);
			gTick.sleptForFrameCap = true;
			if (forDay)
			{
				gTick.dayWakes++;
				return kAnotherPass;
			}
			return kLeave;
		}

		// Returns kContinue, kLeave or kAnotherPass.
		int __stdcall OnPass(const uint8_t* sim, uint32_t elapsed)
		{
			if (!gTick.active) return kContinue;
			gLastPassQpc = QpcNow();   // the last pass of a slice marks where the slice ended

			if (!gTick.paused && sim[0xD8] != 0) gWindow.simDays++;

			bool const caughtUp = CaughtUp(sim);
			uint32_t const delta = elapsed >= gTick.lastElapsed ? elapsed - gTick.lastElapsed : 0;
			if (gTick.prevCaughtUp && caughtUp) gTick.idleOnlyMs += delta;
			gTick.prevCaughtUp = caughtUp;
			gTick.lastElapsed = elapsed;
			gTick.passes++;

			if (!caughtUp)
			{
				gTick.caughtUpAt = kNotCaughtUp;
				return kContinue;
			}
			if (gTick.caughtUpAt == kNotCaughtUp) gTick.caughtUpAt = elapsed;
			if (gOptions.earlyExitMs < 0) return kContinue;
			if (elapsed - gTick.caughtUpAt < static_cast<uint32_t>(gOptions.earlyExitMs)) return kContinue;

			if (gMinFrameTicks > 0)
			{
				int const result = SleepRestOfFrame(sim);
				gLastPassQpc = QpcNow();   // the sleep is not time outside the slice
				if (result == kAnotherPass) return kAnotherPass;
			}

			gTick.exitedEarly = true;
			return kLeave;
		}

		// Loop entry: displaced lea ebx, [esi+0x190].
		__declspec(naked) void EntryStub()
		{
			__asm {
				push eax
				push ecx
				push edx
				push esi
				call OnTickBegin
				pop  edx
				pop  ecx
				pop  eax
				lea  ebx, [esi + 0x190]
				jmp  dword ptr [gEntryRejoin]
			}
		}

		// Loop tail: displaced mov ecx, ebx; call cRZTimer::GetElapsedTime.
		// EAX (elapsed) must survive to the cmp at the rejoin, except that 0 forces
		// another pass; ECX/EDX are dead.
		__declspec(naked) void TailStub()
		{
			__asm {
				mov  ecx, ebx
				call dword ptr [gGetElapsed]
				push eax
				push eax
				push esi
				call OnPass
				mov  ecx, eax
				pop  eax
				cmp  ecx, 1            // kLeave
				je   leave_slice
				cmp  ecx, 2            // kAnotherPass
				jne  rejoin
				xor  eax, eax
			rejoin:
				jmp  dword ptr [gTailRejoin]
			leave_slice:
				jmp  dword ptr [gLoopExit]
			}
		}

		bool WriteJump(Patch& patch, uint8_t* target, size_t length, const uint8_t* expected, void* stub)
		{
			if (std::memcmp(target, expected, length) != 0) return false;
			DWORD prot = 0;
			if (!VirtualProtect(target, length, PAGE_EXECUTE_READWRITE, &prot)) return false;
			std::memcpy(patch.saved, target, length);
			target[0] = 0xE9;
			int32_t const disp = static_cast<int32_t>(reinterpret_cast<uintptr_t>(stub) -
				(reinterpret_cast<uintptr_t>(target) + 5));
			std::memcpy(target + 1, &disp, sizeof(disp));
			std::memset(target + 5, 0x90, length - 5);
			DWORD ignored = 0;
			VirtualProtect(target, length, prot, &ignored);
			FlushInstructionCache(GetCurrentProcess(), target, length);
			patch.target = target;
			patch.length = length;
			return true;
		}

		void Restore(Patch& patch)
		{
			if (patch.target == nullptr) return;
			DWORD prot = 0;
			if (VirtualProtect(patch.target, patch.length, PAGE_EXECUTE_READWRITE, &prot))
			{
				std::memcpy(patch.target, patch.saved, patch.length);
				DWORD ignored = 0;
				VirtualProtect(patch.target, patch.length, prot, &ignored);
				FlushInstructionCache(GetCurrentProcess(), patch.target, patch.length);
			}
			patch = Patch{};
		}

	} // namespace

	namespace
	{
		bool IsOff(const char* value)
		{
			return _stricmp(value, "off") == 0 || _stricmp(value, "no") == 0 || _stricmp(value, "false") == 0;
		}

		// Returns false when value is not a number.
		bool ParseNumber(const char* value, long& out)
		{
			char* end = nullptr;
			out = std::strtol(value, &end, 10);
			return end != value;
		}
	}

	int ParseEarlyExit(const char* value)
	{
		if (value == nullptr || *value == '\0') return kDefaultEarlyExitMs;
		if (IsOff(value)) return -1;
		long ms = 0;
		if (!ParseNumber(value, ms)) return kDefaultEarlyExitMs;
		if (ms < 0) return -1;
		return ms > kMaxEarlyExitMs ? kMaxEarlyExitMs : static_cast<int>(ms);
	}

	int ParseFrameCap(const char* value)
	{
		if (value == nullptr || *value == '\0' || IsOff(value)) return 0;
		long fps = 0;
		if (!ParseNumber(value, fps) || fps <= 0) return 0;
		if (fps < kMinFrameCapFps) return kMinFrameCapFps;
		return fps > kMaxFrameCapFps ? kMaxFrameCapFps : static_cast<int>(fps);
	}

	bool Install(const Options& options)
	{
		if (gInstalled) return true;
		Logger& logger = Logger::GetInstance();

		if (options.earlyExitMs < 0 && !options.probe)
		{
			logger.WriteLine(LogLevel::Info, "sim early exit: turned off");
			return false;
		}
		if (!GameVersion::IsSupportedForPatches())
		{
			logger.WriteLine(LogLevel::Info, "sim early exit: requires SimCity 4 1.1.641, skipping");
			return false;
		}

		auto module = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
		if (module == nullptr) return false;
		gEntryRejoin = reinterpret_cast<uintptr_t>(module + (kEntryRejoinVA - kImageBase));
		gTailRejoin = reinterpret_cast<uintptr_t>(module + (kTailRejoinVA - kImageBase));
		gLoopExit = reinterpret_cast<uintptr_t>(module + (kLoopExitVA - kImageBase));
		gGetElapsed = reinterpret_cast<uintptr_t>(module + (kGetElapsedVA - kImageBase));
		gGetElapsedFn = reinterpret_cast<GetElapsedFn>(gGetElapsed);
		gOptions = options;

		LARGE_INTEGER frequency;
		if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) return false;
		gQpcMsPerTick = 1000.0 / static_cast<double>(frequency.QuadPart);

		// The frame cap sleeps only after an early exit decision.
		bool const frameCap = options.frameCapFps > 0 && options.earlyExitMs >= 0;
		if (frameCap)
		{
			if (!CreateFrameTimer())
			{
				logger.WriteLine(LogLevel::Info, "sim frame cap: no waitable timer, sleeping with Sleep()");
			}
			gMinFrameTicks = frequency.QuadPart / options.frameCapFps;
		}

		if (!WriteJump(gTailPatch, module + (kTailVA - kImageBase), kTailLen, kTailExpected, &TailStub))
		{
			DestroyFrameTimer();
			gMinFrameTicks = 0;
			logger.WriteLine(LogLevel::Info, "sim early exit: cSC4Simulator::OnTick loop tail already modified, skipping");
			return false;
		}
		if (!WriteJump(gEntryPatch, module + (kEntryVA - kImageBase), kEntryLen, kEntryExpected, &EntryStub))
		{
			Restore(gTailPatch);
			DestroyFrameTimer();
			gMinFrameTicks = 0;
			logger.WriteLine(LogLevel::Info, "sim early exit: cSC4Simulator::OnTick loop entry already modified, skipping");
			return false;
		}

		gInstalled = true;
		if (options.earlyExitMs >= 0)
		{
			char frameCapText[64];
			if (frameCap)
			{
				std::snprintf(frameCapText, sizeof(frameCapText), "%d fps (%s timer)", options.frameCapFps,
					gTimerHighRes ? "high-resolution" : gTimePeriodRaised ? "1 ms resolution" : "default resolution");
			}
			else
			{
				std::snprintf(frameCapText, sizeof(frameCapText), "off");
			}
			logger.WriteLineFormatted(LogLevel::Info,
				"sim early exit installed: idle allowance=%d ms, frame cap=%s%s",
				options.earlyExitMs, frameCapText, options.probe ? ", probe on" : "");
		}
		else
		{
			logger.WriteLine(LogLevel::Info, "sim early exit off, probe installed");
		}
		return true;
	}

	void Uninstall()
	{
		if (!gInstalled) return;
		// Entry first, so no tick starts against a removed tail.
		Restore(gEntryPatch);
		Restore(gTailPatch);
		gMinFrameTicks = 0;
		DestroyFrameTimer();
		gInstalled = false;
		Logger::GetInstance().WriteLine(LogLevel::Info, "sim early exit uninstalled");
	}

} // namespace SimSlice
