// pthread_setaffinity_np / CPU_SET / CPU_ZERO are GNU extensions on glibc and
// are only declared when _GNU_SOURCE is visible to <sched.h>/<pthread.h>.
// The project currently builds with GNU extensions enabled by default
// (CMAKE_CXX_EXTENSIONS defaults to ON, i.e. -std=gnu++20), which predefines
// this automatically - but we don't want this file's compilability to depend
// on that default never changing (e.g. a stricter CEMU_CXX_FLAGS override),
// so define it explicitly. Must come before any system header is pulled in,
// including transitively via CpuAffinity.h/precompiled.h.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "Common/CpuAffinity.h"
#include "Common/precompiled.h"

#include <mutex>
#include <algorithm>
#include <fstream>
#include <string>
#include <cerrno>

#if BOOST_OS_LINUX || BOOST_PLAT_ANDROID
#include <sched.h>
#include <pthread.h>
#include <sys/resource.h>
#include <unistd.h>
#define CPUAFFINITY_HAS_LINUX_API 1
#else
#define CPUAFFINITY_HAS_LINUX_API 0
#endif

namespace
{
	std::once_flag s_detectOnce;
	bool s_isHeterogeneous = false;
	bool s_detectionValid = false; // true if we managed to read frequencies for at least one core
	std::vector<uint32_t> s_fastClusterCores;
	uint32_t s_totalCoreCount = 0;

#if CPUAFFINITY_HAS_LINUX_API
	// Reads a single integer value from a sysfs file. Returns -1 on failure
	// (missing file, permission denied, garbage content, etc - all of which
	// happen in practice across the Android vendor landscape).
	long ReadSysfsInt(const std::string& path)
	{
		std::ifstream f(path);
		if (!f.is_open())
			return -1;
		long value = -1;
		f >> value;
		if (f.fail())
			return -1;
		return value;
	}

	// Ranks logical CPUs by a relative "speed" figure, one entry per core
	// index that was successfully read.
	//
	// We prefer /sys/devices/system/cpu/cpu<N>/cpu_capacity when available:
	// it's a static relative-performance figure populated from the device
	// tree at boot (e.g. 180 for a little core, 1024 for the fastest core)
	// and, critically, it stays readable even while a core is hotplugged
	// offline. cpuinfo_max_freq/scaling_max_freq, by contrast, live under
	// the cpufreq policy directory, which on many Android SoCs disappears
	// for a core (or a whole cluster) while it's powered down - and cores
	// are very commonly still offline at the exact moment our JIT/PPC/GPU
	// threads start up (right after process launch, before the load hits).
	// Relying on cpufreq alone would then read only the online (little)
	// cores, see a single uniform value, and permanently misdetect the
	// device as homogeneous for the rest of the session.
	// We only mix in the cpufreq fallback for cores that have no capacity
	// entry at all, and only if not a single core exposed cpu_capacity -
	// the two figures are on different scales, so partial mixing would
	// corrupt the comparison.
	std::vector<std::pair<uint32_t, long>> ReadPerCoreMaxFrequencies(uint32_t coreCount)
	{
		std::vector<std::pair<uint32_t, long>> capacityResult;
		std::vector<std::pair<uint32_t, long>> freqResult;
		for (uint32_t i = 0; i < coreCount; i++)
		{
			std::string base = fmt::format("/sys/devices/system/cpu/cpu{}/", i);
			long capacity = ReadSysfsInt(base + "cpu_capacity");
			if (capacity > 0)
				capacityResult.emplace_back(i, capacity);

			long freq = ReadSysfsInt(base + "cpufreq/cpuinfo_max_freq");
			if (freq <= 0)
				freq = ReadSysfsInt(base + "cpufreq/scaling_max_freq"); // fallback if cpuinfo_* is hidden
			if (freq > 0)
				freqResult.emplace_back(i, freq);
		}
		// Prefer capacity data whenever we got at least one reading from it -
		// it's strictly more reliable for our purposes (survives hotplug).
		if (!capacityResult.empty())
			return capacityResult;
		return freqResult;
	}

	void DetectTopologyImpl()
	{
		long confCores = sysconf(_SC_NPROCESSORS_CONF);
		if (confCores <= 0)
			confCores = 16; // generous fallback upper bound for the probe loop
		s_totalCoreCount = (uint32_t)confCores;

		auto perCore = ReadPerCoreMaxFrequencies((uint32_t)confCores);
		if (perCore.empty())
		{
			// Could not read anything (permission denied / non-Linux sysfs
			// layout / container sandboxing). Fail safe: treat as
			// homogeneous so every other function becomes a no-op rather
			// than risking a bad pin.
			s_detectionValid = false;
			s_isHeterogeneous = false;
			cemuLog_log(LogType::Force, "CpuAffinity: could not read per-core topology info, skipping core pinning");
			return;
		}
		s_detectionValid = true;

		long maxFreq = 0;
		for (auto& [idx, freq] : perCore)
			maxFreq = std::max(maxFreq, freq);

		// Consider a core part of the "fast" cluster if its max frequency is
		// within 10% of the single fastest core on the chip. This groups
		// together prime + big cores (which are usually close to each other,
		// e.g. 3.2GHz prime / 2.8GHz big) while excluding little cores, which
		// are typically 30-45% slower on current mobile SoCs.
		constexpr double kFastClusterThreshold = 0.90;
		for (auto& [idx, freq] : perCore)
		{
			if ((double)freq >= (double)maxFreq * kFastClusterThreshold)
				s_fastClusterCores.push_back(idx);
		}

		// Heterogeneous only if the fast cluster doesn't already cover every
		// detected core (i.e. there really is a slower cluster to avoid).
		s_isHeterogeneous = s_fastClusterCores.size() < perCore.size();

		if (s_isHeterogeneous)
		{
			cemuLog_log(LogType::Force, "CpuAffinity: detected heterogeneous SoC ({} of {} cores in fast cluster, top rank value {})",
				s_fastClusterCores.size(), perCore.size(), maxFreq);
		}
		else
		{
			cemuLog_log(LogType::Force, "CpuAffinity: CPU appears homogeneous ({} cores), core pinning disabled", perCore.size());
		}
	}
#else
	void DetectTopologyImpl()
	{
		// No affinity API on this platform (e.g. macOS, Windows-on-ARM
		// heterogeneous handling is left to the OS scheduler which already
		// does a reasonable job there). Stay a no-op.
		s_detectionValid = false;
		s_isHeterogeneous = false;
	}
#endif
}

void CpuAffinity::EnsureTopologyDetected()
{
	std::call_once(s_detectOnce, DetectTopologyImpl);
}

bool CpuAffinity::IsHeterogeneous()
{
	EnsureTopologyDetected();
	return s_isHeterogeneous;
}

uint32_t CpuAffinity::GetFastClusterCoreCount()
{
	EnsureTopologyDetected();
	return (uint32_t)s_fastClusterCores.size();
}

uint32_t CpuAffinity::GetTotalCoreCount()
{
	EnsureTopologyDetected();
	return s_totalCoreCount;
}

const std::vector<uint32_t>& CpuAffinity::GetFastClusterCores()
{
	EnsureTopologyDetected();
	return s_fastClusterCores;
}

void CpuAffinity::PinCurrentThreadToCores(const std::vector<uint32_t>& cores)
{
	if (cores.empty())
		return;
#if CPUAFFINITY_HAS_LINUX_API
	cpu_set_t cpuSet;
	CPU_ZERO(&cpuSet);
	for (uint32_t core : cores)
		CPU_SET((int)core, &cpuSet);
	// Best-effort: some Android vendors sandbox sched_setaffinity for
	// non-system apps on certain cpusets. Ignore failures rather than
	// asserting - worst case we simply fall back to default placement.
	#if defined(__BIONIC__)
		sched_setaffinity(0, sizeof(cpu_set_t), &cpuSet);
#else
		pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuSet);
#endif
#else
	(void)cores;
#endif
}

void CpuAffinity::NudgeCurrentThreadPriority(int32_t delta)
{
#if CPUAFFINITY_HAS_LINUX_API
	errno = 0;
	int currentNice = getpriority(PRIO_PROCESS, 0);
	if (errno != 0)
		return;
	int wanted = std::clamp(currentNice + (int)delta, -20, 19);
	setpriority(PRIO_PROCESS, 0, wanted); // ignore failure - not all Android cpusets permit negative nice for app threads
#else
	(void)delta;
#endif
}

void CpuAffinity::PinCurrentThread(ThreadRole role)
{
	EnsureTopologyDetected();
	if (!s_detectionValid || !s_isHeterogeneous || s_fastClusterCores.empty())
		return; // nothing to do - homogeneous CPU or detection failed

	PinCurrentThreadToCores(s_fastClusterCores);

	// Priority nudges are intentionally small. The goal is to win ties
	// against background/UI work when the fast cluster is momentarily
	// oversubscribed (e.g. 3 PPC cores + 2 JIT workers sharing 4 fast
	// cores during a loading screen), not to starve the rest of the app.
	switch (role)
	{
	case ThreadRole::EmulatedCpuCore:
		NudgeCurrentThreadPriority(-4);
		break;
	case ThreadRole::GpuSubmission:
		NudgeCurrentThreadPriority(-4);
		break;
	case ThreadRole::JitCompiler:
		NudgeCurrentThreadPriority(-2); // slightly lower boost - must not outrank the emulated cores it feeds
		break;
	}
}
