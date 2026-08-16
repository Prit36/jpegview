#pragma once

#include <windows.h>
#include <span>
#include <string>

// RAII zero-copy memory-mapped file reader for high-throughput image reading
class CMemoryMappedFile {
public:
    CMemoryMappedFile() = default;
    explicit CMemoryMappedFile(const std::wstring& filePath) {
        Open(filePath);
    }

    ~CMemoryMappedFile() {
        Close();
    }

    // Move-only semantics
    CMemoryMappedFile(const CMemoryMappedFile&) = delete;
    CMemoryMappedFile& operator=(const CMemoryMappedFile&) = delete;

    CMemoryMappedFile(CMemoryMappedFile&& other) noexcept 
        : m_hFile(other.m_hFile), m_hMapping(other.m_hMapping),
          m_pData(other.m_pData), m_size(other.m_size) {
        other.m_hFile = INVALID_HANDLE_VALUE;
        other.m_hMapping = nullptr;
        other.m_pData = nullptr;
        other.m_size = 0;
    }

    CMemoryMappedFile& operator=(CMemoryMappedFile&& other) noexcept {
        if (this != &other) {
            Close();
            m_hFile = other.m_hFile;
            m_hMapping = other.m_hMapping;
            m_pData = other.m_pData;
            m_size = other.m_size;

            other.m_hFile = INVALID_HANDLE_VALUE;
            other.m_hMapping = nullptr;
            other.m_pData = nullptr;
            other.m_size = 0;
        }
        return *this;
    }

    bool Open(const std::wstring& filePath) {
        Close();

        m_hFile = ::CreateFileW(
            filePath.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr
        );

        if (m_hFile == INVALID_HANDLE_VALUE) {
            return false;
        }

        LARGE_INTEGER fileSize;
        if (!::GetFileSizeEx(m_hFile, &fileSize) || fileSize.QuadPart == 0) {
            Close();
            return false;
        }

        m_size = static_cast<size_t>(fileSize.QuadPart);

        m_hMapping = ::CreateFileMappingW(
            m_hFile,
            nullptr,
            PAGE_READONLY,
            0,
            0,
            nullptr
        );

        if (!m_hMapping) {
            Close();
            return false;
        }

        m_pData = ::MapViewOfFile(m_hMapping, FILE_MAP_READ, 0, 0, 0);
        if (!m_pData) {
            Close();
            return false;
        }

        return true;
    }

    void Close() {
        if (m_pData) {
            ::UnmapViewOfFile(m_pData);
            m_pData = nullptr;
        }
        if (m_hMapping) {
            ::CloseHandle(m_hMapping);
            m_hMapping = nullptr;
        }
        if (m_hFile != INVALID_HANDLE_VALUE) {
            ::CloseHandle(m_hFile);
            m_hFile = INVALID_HANDLE_VALUE;
        }
        m_size = 0;
    }

    bool IsOpen() const noexcept { return m_pData != nullptr; }
    const uint8_t* Data() const noexcept { return static_cast<const uint8_t*>(m_pData); }
    size_t Size() const noexcept { return m_size; }
    std::span<const uint8_t> Bytes() const noexcept { return { Data(), m_size }; }

private:
    HANDLE m_hFile = INVALID_HANDLE_VALUE;
    HANDLE m_hMapping = nullptr;
    void* m_pData = nullptr;
    size_t m_size = 0;
};
