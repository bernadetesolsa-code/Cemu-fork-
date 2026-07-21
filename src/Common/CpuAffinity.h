#pragma once

// Thread placement helpers for heterogeneous (big.LITTLE / DynamIQ) SoCs.
//
// Background: on Android almost every SoC exposes a mix of core types (e.g.
// 1 "prime" + 3 "big" + 4 "little" on a typical mid-to-high-end chip). The
// Linux scheduler on these devices strongly favors power efficiency and will
// happily migrate a hot emulation thread (PPC core, JIT worker, GPU command
// submission) onto a "little" core to save power - especially right after
// the thread is created, before the scheduler has learned it's CPU-bound.
// The result is a performance lottery: same hardware, same game, different
// framerate from one session to the next depending on where the scheduler
// happened to place things.
//
// This module reads /sys/devices/system/cpu/cpu*/cpu_capacity (falling back
// to cpufreq's cpuinfo_max_freq when capacity isn't exposed) to rank cores by
// relative performance - the same technique used by other Android emulators
// such as Dolphin/Yuzu - and exposes a small API to pin the process's hot
// threads to the fastest cluster and to nudge their scheduling priority. On a homogeneous system (desktop Linux, or an Android device
// that fails to expose cpufreq for some reason) the "fast" set simply
// degenerates to "all cores", making every call in here a safe no-op.
//
// This is scheduling-only: it does not touch emulation logic, and it is
// intentionally conservative - we never pin to a single core, only to a
// cluster, so the OS scheduler still has freedom to load-balance within the
// fast set.

#include <vector>
#include <cstdint>

class CpuAffinity
{
public:
	// Roles used purely to select a sensible priority nudge per call site.
	// Affinity target is always "the fastest cluster" regardless of role.
	enum class ThreadRole
	{
		EmulatedCpuCore,  // one of the 3 emulated PPC cores (coreinit scheduler threads)
		JitCompiler,      // PPCRecompiler worker thread
		GpuSubmission,    // Latte / GX2 command processing thread
	};

	// Detects CPU topology once (thread-safe, cheap to call repeatedly after
	// the first call). Safe to call multiple times; only the first call does
	// real work. Called automatically by PinCurrentThread() if needed, but
	// callers may invoke it early (e.g. at startup) to log detected topology.
	static void EnsureTopologyDetected();

	// True if the detector found more than one distinct core "speed" bucket,
	// i.e. the device is a big.LITTLE / heterogeneous SoC where pinning
	// actually matters. False on uniform desktop/server CPUs or if detection
	// failed (in which case all calls below become no-ops).
	static bool IsHeterogeneous();

	// Number of core buckets found in total, and the number of cores
	// belonging to the fastest bucket (mainly useful for diagnostics/logging).
	static uint32_t GetFastClusterCoreCount();
	static uint32_t GetTotalCoreCount();

	// Pins the *calling* thread's affinity mask to the fastest core cluster
	// (prime + big cores, i.e. every core sharing the highest observed
	// cpuinfo_max_freq bucket, plus any bucket within ~90% of it to also
	// capture prime/big splits that report slightly different max freqs).
	// Also applies a small niceness boost appropriate for `role`.
	// No-op (returns without touching anything) if topology detection failed
	// or the device is homogeneous.
	static void PinCurrentThread(ThreadRole role);

	// Lower-level building block: restricts the calling thread to the given
	// set of logical CPU indices. No-op on platforms without affinity APIs
	// (e.g. macOS) or if `cores` is empty.
	static void PinCurrentThreadToCores(const std::vector<uint32_t>& cores);

	// Raises (or lowers) the calling thread's scheduling niceness by `delta`
	// (negative = higher priority). Best-effort; failures are silently
	// ignored since apps are not guaranteed elevated scheduling rights on
	// all Android vendors/versions.
	static void NudgeCurrentThreadPriority(int32_t delta);

	// Returns the fastest-cluster core list detected (empty if homogeneous
	// or detection failed). Exposed mainly for logging/diagnostics.
	static const std::vector<uint32_t>& GetFastClusterCores();
};
