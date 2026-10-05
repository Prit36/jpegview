#pragma once

#include <windows.h>
#include <thread>

namespace ProcessorCount {
// Count CPUs available to this process, rather than all CPUs in the host.
// Job Object affinity and user-selected affinity are inherited by workers.
inline unsigned int Available() {
	DWORD_PTR processMask = 0, systemMask = 0;
	if (::GetProcessAffinityMask(::GetCurrentProcess(), &processMask, &systemMask) && processMask) {
		unsigned int count = 0;
		while (processMask) {
			processMask &= processMask - 1;
			++count;
		}
		return count;
	}
	unsigned int count = std::thread::hardware_concurrency();
	if (count) return count;
	SYSTEM_INFO info;
	::GetSystemInfo(&info);
	return info.dwNumberOfProcessors ? info.dwNumberOfProcessors : 1;
}
}
