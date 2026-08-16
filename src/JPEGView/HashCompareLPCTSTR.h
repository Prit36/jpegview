#pragma once

#include <cstddef>
#include <tchar.h>

// For high-performance inlined usage in std::unordered_map
struct CHashLPCTSTR
{
	inline size_t operator()(LPCTSTR Key) const noexcept {
		if (!Key) return 0;
		size_t nHash = 0;
		int nCnt = 0;
		while (Key[nCnt] != 0 && nCnt++ < 16) {
			nHash += Key[nCnt];
			nHash = (nHash << 8) + nHash;
		}
		return nHash;
	}
};

struct CEqualLPCTSTR
{
	inline bool operator()(LPCTSTR Key1, LPCTSTR Key2) const noexcept {
		if (Key1 == Key2) return true;
		if (!Key1 || !Key2) return false;
		return _tcscmp(Key1, Key2) == 0;
	}
};