#include "stdafx.h"
#include "GpuHeifDecoder.h"
#include "MaxImageDef.h"
#include "ICCProfileTransform.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <d3d11_4.h>
#include <dxgi1_4.h>

#include <vector>
#include <map>
#include <string>
#include <algorithm>
#include <immintrin.h>
#include <omp.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "Ole32.lib")

namespace {

// YCbCr -> RGB coefficients
struct YCbCrCoefficients {
	float r_cr;
	float g_cb;
	float g_cr;
	float b_cb;
};

static YCbCrCoefficients GetNclxCoefficients(uint16_t matrix_coefficients) {
	float Kr = 0.0f, Kb = 0.0f;
	switch (matrix_coefficients) {
		case 1:  Kr = 0.2126f; Kb = 0.0722f; break; // BT.709
		case 4:  Kr = 0.30f;   Kb = 0.11f;   break; // FCC
		case 5:
		case 6:  Kr = 0.299f;  Kb = 0.114f;  break; // BT.601
		case 7:  Kr = 0.212f;  Kb = 0.087f;  break; // SMPTE 240M
		case 9:
		case 10: Kr = 0.2627f; Kb = 0.0593f; break; // BT.2020
		default: break;
	}
	if (Kr == 0.0f && Kb == 0.0f) {
		// Default BT.601
		return { 1.402f, -0.344136f, -0.714136f, 1.772f };
	}
	YCbCrCoefficients c;
	c.r_cr = 2.0f * (1.0f - Kr);
	c.g_cb = 2.0f * Kb * (1.0f - Kb) / (Kb + Kr - 1.0f);
	c.g_cr = 2.0f * Kr * (1.0f - Kr) / (Kb + Kr - 1.0f);
	c.b_cb = 2.0f * (1.0f - Kb);
	return c;
}

// Media Foundation GPU Decoder Manager
class MfGpuContext {
public:
	ID3D11Device* m_pDevice = nullptr;
	ID3D11DeviceContext* m_pContext = nullptr;
	IMFDXGIDeviceManager* m_pDXGIManager = nullptr;
	IMFActivate* m_pDecoderActivate = nullptr;
	UINT m_resetToken = 0;
	bool m_initialized = false;
	bool m_supportChecked = false;
	bool m_hasHwDecoder = false;

	static MfGpuContext& Instance() {
		static MfGpuContext ctx;
		return ctx;
	}

	bool EnsureInit() {
		if (m_initialized) return true;
		if (m_supportChecked && !m_hasHwDecoder) return false;

		MFStartup(MF_VERSION);

		D3D_FEATURE_LEVEL fl;
		HRESULT hr = D3D11CreateDevice(
			nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
			D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
			nullptr, 0, D3D11_SDK_VERSION,
			&m_pDevice, &fl, &m_pContext
		);
		if (FAILED(hr) || !m_pDevice) {
			m_supportChecked = true;
			m_hasHwDecoder = false;
			return false;
		}

		ID3D11Multithread* pMultithread = nullptr;
		if (SUCCEEDED(m_pDevice->QueryInterface(__uuidof(ID3D11Multithread), (void**)&pMultithread))) {
			pMultithread->SetMultithreadProtected(TRUE);
			pMultithread->Release();
		}

		hr = MFCreateDXGIDeviceManager(&m_resetToken, &m_pDXGIManager);
		if (FAILED(hr) || !m_pDXGIManager) {
			m_supportChecked = true;
			m_hasHwDecoder = false;
			return false;
		}

		hr = m_pDXGIManager->ResetDevice(m_pDevice, m_resetToken);
		if (FAILED(hr)) {
			m_supportChecked = true;
			m_hasHwDecoder = false;
			return false;
		}

		MFT_REGISTER_TYPE_INFO inputType = { MFMediaType_Video, MFVideoFormat_HEVC };
		IMFActivate** ppActivate = NULL;
		UINT32 count = 0;
		hr = MFTEnumEx(
			MFT_CATEGORY_VIDEO_DECODER,
			MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
			&inputType, NULL, &ppActivate, &count
		);

		if (count == 0 || FAILED(hr)) {
			m_supportChecked = true;
			m_hasHwDecoder = false;
			return false;
		}

		m_pDecoderActivate = ppActivate[0];
		m_pDecoderActivate->AddRef();
		for (UINT32 i = 0; i < count; i++) ppActivate[i]->Release();
		CoTaskMemFree(ppActivate);

		m_hasHwDecoder = true;
		m_supportChecked = true;
		m_initialized = true;
		return true;
	}

	bool DecodeHevcBitstream(
		const uint8_t* bitstream, size_t bitstreamSize,
		uint32_t width, uint32_t height,
		std::vector<uint8_t>& outNV12, uint32_t& outStride)
	{
		if (!EnsureInit()) return false;

		IMFTransform* pDecoder = nullptr;
		HRESULT hr = m_pDecoderActivate->ActivateObject(IID_IMFTransform, (void**)&pDecoder);
		if (FAILED(hr) || !pDecoder) return false;

		hr = pDecoder->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)m_pDXGIManager);

		IMFMediaType* pInType = nullptr;
		MFCreateMediaType(&pInType);
		pInType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		pInType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_HEVC);
		MFSetAttributeSize(pInType, MF_MT_FRAME_SIZE, width, height);
		hr = pDecoder->SetInputType(0, pInType, 0);
		pInType->Release();
		if (FAILED(hr)) { pDecoder->Release(); return false; }

		IMFMediaType* pOutType = nullptr;
		MFCreateMediaType(&pOutType);
		pOutType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		pOutType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
		MFSetAttributeSize(pOutType, MF_MT_FRAME_SIZE, width, height);
		hr = pDecoder->SetOutputType(0, pOutType, 0);
		pOutType->Release();
		if (FAILED(hr)) { pDecoder->Release(); return false; }

		pDecoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
		pDecoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

		IMFSample* pInSample = nullptr;
		MFCreateSample(&pInSample);
		IMFMediaBuffer* pInBuf = nullptr;
		MFCreateMemoryBuffer((DWORD)bitstreamSize, &pInBuf);
		BYTE* pDst = nullptr;
		pInBuf->Lock(&pDst, NULL, NULL);
		memcpy(pDst, bitstream, bitstreamSize);
		pInBuf->Unlock();
		pInBuf->SetCurrentLength((DWORD)bitstreamSize);
		pInSample->AddBuffer(pInBuf);
		pInSample->SetSampleTime(0);
		pInSample->SetSampleDuration(1);

		hr = pDecoder->ProcessInput(0, pInSample, 0);
		pInSample->Release();
		pInBuf->Release();
		if (FAILED(hr)) { pDecoder->Release(); return false; }

		pDecoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);

		MFT_OUTPUT_STREAM_INFO osi = { 0 };
		pDecoder->GetOutputStreamInfo(0, &osi);

		MFT_OUTPUT_DATA_BUFFER outputBuffer = { 0 };
		DWORD status = 0;
		bool success = false;

		for (int iter = 0; iter < 10; iter++) {
			outputBuffer.pSample = nullptr;
			if (!(osi.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
				IMFSample* pOutSample = nullptr;
				MFCreateSample(&pOutSample);
				IMFMediaBuffer* pOutBuf = nullptr;
				MFCreateMemoryBuffer(osi.cbSize, &pOutBuf);
				pOutSample->AddBuffer(pOutBuf);
				pOutBuf->Release();
				outputBuffer.pSample = pOutSample;
			}

			hr = pDecoder->ProcessOutput(0, 1, &outputBuffer, &status);
			if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
				IMFMediaType* pAvailableType = nullptr;
				hr = pDecoder->GetOutputAvailableType(0, 0, &pAvailableType);
				if (SUCCEEDED(hr)) {
					pDecoder->SetOutputType(0, pAvailableType, 0);
					pAvailableType->Release();
				}
				pDecoder->GetOutputStreamInfo(0, &osi);
				continue;
			}

			if (SUCCEEDED(hr) && outputBuffer.pSample) {
				IMFMediaBuffer* pBuf = nullptr;
				outputBuffer.pSample->GetBufferByIndex(0, &pBuf);
				if (pBuf) {
					BYTE* pRaw = nullptr;
					DWORD curLen = 0;
					pBuf->Lock(&pRaw, NULL, &curLen);
					outStride = width;
					outNV12.assign(pRaw, pRaw + curLen);
					pBuf->Unlock();
					pBuf->Release();
					success = true;
				}
				outputBuffer.pSample->Release();
				break;
			}

			if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
		}

		pDecoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
		pDecoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
		pDecoder->Release();

		return success;
	}

private:
	MfGpuContext() {}
	~MfGpuContext() {
		if (m_pDecoderActivate) m_pDecoderActivate->Release();
		if (m_pDXGIManager) m_pDXGIManager->Release();
		if (m_pContext) m_pContext->Release();
		if (m_pDevice) m_pDevice->Release();
		if (m_initialized) MFShutdown();
	}
};

// ISOBMFF Structures
struct IsoItem {
	uint32_t id = 0;
	std::string type;
	uint32_t width = 0;
	uint32_t height = 0;
	uint64_t offset = 0;
	uint64_t length = 0;
	std::vector<uint8_t> hvcC_data;
	std::vector<uint8_t> icc_profile;
	uint16_t matrix_coefficients = 1; // BT.709 default
	bool full_range = true;
	bool has_nclx = false;
};

struct IsoGrid {
	uint8_t rows = 0;
	uint8_t columns = 0;
	uint32_t outputWidth = 0;
	uint32_t outputHeight = 0;
	std::vector<uint32_t> tileItemIds;
};

class IsoHeifDemuxer {
public:
	const uint8_t* m_data = nullptr;
	size_t m_size = 0;
	size_t m_idatOffset = 0;
	size_t m_idatLength = 0;
	std::map<uint32_t, IsoItem> m_items;
	std::map<uint32_t, IsoGrid> m_grids;
	std::vector<uint32_t> m_topLevelItemIds;
	uint32_t m_primaryItemId = 0;
	uint32_t m_exifItemId = 0;

	bool Parse(const uint8_t* data, size_t size) {
		m_data = data;
		m_size = size;
		m_idatOffset = 0;
		m_idatLength = 0;
		m_items.clear();
		m_grids.clear();
		m_topLevelItemIds.clear();
		m_primaryItemId = 0;
		m_exifItemId = 0;

		size_t offset = 0;
		while (offset + 8 <= size) {
			uint64_t boxSize = ReadU32(offset);
			std::string boxType((const char*)&m_data[offset + 4], 4);
			size_t headerSize = 8;
			if (boxSize == 1) {
				if (offset + 16 > size) break;
				boxSize = ReadU64(offset + 8);
				headerSize = 16;
			} else if (boxSize == 0) {
				boxSize = size - offset;
			}

			if (boxType == "meta") {
				ParseMeta(offset + headerSize + 4, boxSize - headerSize - 4);
			}
			offset += boxSize;
		}

		if (m_primaryItemId != 0) {
			m_topLevelItemIds.push_back(m_primaryItemId);
		}
		for (const auto& kv : m_items) {
			if (kv.second.type == "hvc1" || kv.second.type == "grid") {
				if (std::find(m_topLevelItemIds.begin(), m_topLevelItemIds.end(), kv.first) == m_topLevelItemIds.end()) {
					// Check if this item is a tile in a grid
					bool isTile = false;
					for (const auto& g : m_grids) {
						if (std::find(g.second.tileItemIds.begin(), g.second.tileItemIds.end(), kv.first) != g.second.tileItemIds.end()) {
							isTile = true;
							break;
						}
					}
					if (!isTile) {
						m_topLevelItemIds.push_back(kv.first);
					}
				}
			}
		}

		return !m_items.empty();
	}

private:
	uint32_t ReadU32(size_t off) const {
		if (off + 4 > m_size) return 0;
		return (m_data[off] << 24) | (m_data[off + 1] << 16) | (m_data[off + 2] << 8) | m_data[off + 3];
	}
	uint64_t ReadU64(size_t off) const {
		if (off + 8 > m_size) return 0;
		uint64_t v = 0;
		for (int i = 0; i < 8; i++) v = (v << 8) | m_data[off + i];
		return v;
	}
	uint16_t ReadU16(size_t off) const {
		if (off + 2 > m_size) return 0;
		return (m_data[off] << 8) | m_data[off + 1];
	}

	void ParseMeta(size_t start, size_t length) {
		size_t end = min(start + length, m_size);
		size_t off = start;

		// First pass: locate idat box if present
		while (off + 8 <= end) {
			uint64_t boxSize = ReadU32(off);
			std::string boxType((const char*)&m_data[off + 4], 4);
			size_t headerSize = 8;
			if (boxSize == 1) {
				boxSize = ReadU64(off + 8);
				headerSize = 16;
			} else if (boxSize == 0) {
				boxSize = end - off;
			}
			if (boxSize < headerSize || off + boxSize > end) break;

			if (boxType == "idat") {
				m_idatOffset = off + headerSize;
				m_idatLength = boxSize - headerSize;
			}
			off += boxSize;
		}

		// Second pass: parse other metadata boxes
		off = start;
		while (off + 8 <= end) {
			uint64_t boxSize = ReadU32(off);
			std::string boxType((const char*)&m_data[off + 4], 4);
			size_t headerSize = 8;
			if (boxSize == 1) {
				boxSize = ReadU64(off + 8);
				headerSize = 16;
			} else if (boxSize == 0) {
				boxSize = end - off;
			}
			if (boxSize < headerSize || off + boxSize > end) break;

			if (boxType == "pitm") {
				uint8_t ver = m_data[off + 8];
				m_primaryItemId = (ver == 0) ? ReadU16(off + 12) : ReadU32(off + 12);
			} else if (boxType == "iinf") {
				ParseIinf(off + headerSize, boxSize - headerSize);
			} else if (boxType == "iloc") {
				ParseIloc(off + headerSize, boxSize - headerSize);
			} else if (boxType == "iprp") {
				ParseIprp(off + headerSize, boxSize - headerSize);
			} else if (boxType == "iref") {
				ParseIref(off + headerSize, boxSize - headerSize);
			}

			off += boxSize;
		}
	}

	void ParseIinf(size_t start, size_t length) {
		if (start + 4 > m_size) return;
		uint8_t ver = m_data[start];
		size_t p = start + 4;
		uint32_t count = (ver == 0) ? ReadU16(p) : ReadU32(p);
		p += (ver == 0) ? 2 : 4;

		for (uint32_t i = 0; i < count && p + 8 <= start + length && p + 8 <= m_size; i++) {
			uint32_t infeSize = ReadU32(p);
			if (infeSize < 8 || p + infeSize > m_size) break;
			std::string infeType((const char*)&m_data[p + 4], 4);
			if (infeType == "infe") {
				uint8_t iVer = m_data[p + 8];
				uint32_t itemId = 0;
				std::string itemType;
				if (iVer >= 2) {
					itemId = (iVer == 2) ? ReadU16(p + 12) : ReadU32(p + 12);
					size_t typeOff = (iVer == 2) ? (p + 16) : (p + 18);
					if (typeOff + 4 <= m_size) {
						itemType = std::string((const char*)&m_data[typeOff], 4);
						m_items[itemId].id = itemId;
						m_items[itemId].type = itemType;
						if (itemType == "Exif") {
							m_exifItemId = itemId;
						}
					}
				}
			}
			p += infeSize;
		}
	}

	void ParseIloc(size_t start, size_t length) {
		if (start + 6 > m_size) return;
		uint8_t ver = m_data[start];
		uint8_t offsetSize = (m_data[start + 4] >> 4) & 0x0F;
		uint8_t lengthSize = m_data[start + 4] & 0x0F;
		uint8_t baseOffsetSize = (m_data[start + 5] >> 4) & 0x0F;
		uint8_t indexSize = (ver >= 1) ? (m_data[start + 5] & 0x0F) : 0;

		size_t p = start + 6;
		uint32_t count = (ver < 2) ? ReadU16(p) : ReadU32(p);
		p += (ver < 2) ? 2 : 4;

		for (uint32_t i = 0; i < count && p < start + length && p < m_size; i++) {
			uint32_t itemId = (ver < 2) ? ReadU16(p) : ReadU32(p);
			p += (ver < 2) ? 2 : 4;
			uint16_t constructionMethod = 0;
			if (ver >= 1) {
				constructionMethod = ReadU16(p);
				p += 2;
			}
			p += 2; // data_reference_index
			uint64_t baseOffset = ReadVarInt(p, baseOffsetSize);
			p += baseOffsetSize;
			uint16_t extentCount = ReadU16(p);
			p += 2;
			for (uint16_t e = 0; e < extentCount; e++) {
				if (ver >= 1 && indexSize > 0) p += indexSize;
				uint64_t extentOffset = ReadVarInt(p, offsetSize);
				p += offsetSize;
				uint64_t extentLength = ReadVarInt(p, lengthSize);
				p += lengthSize;

				uint64_t finalOffset = baseOffset + extentOffset;
				if (constructionMethod == 1) {
					finalOffset += m_idatOffset;
				}
				m_items[itemId].offset = finalOffset;
				m_items[itemId].length = extentLength;
			}
		}
	}

	void ParseIprp(size_t start, size_t length) {
		size_t end = min(start + length, m_size);
		size_t off = start;
		std::vector<std::pair<std::string, std::vector<uint8_t>>> properties;
		properties.push_back({ "", {} });

		while (off + 8 <= end) {
			uint64_t boxSize = ReadU32(off);
			if (boxSize < 8 || off + boxSize > end) break;
			std::string boxType((const char*)&m_data[off + 4], 4);
			if (boxType == "ipco") {
				size_t ipcoOff = off + 8;
				while (ipcoOff + 8 <= off + boxSize) {
					uint32_t propSize = ReadU32(ipcoOff);
					if (propSize < 8 || ipcoOff + propSize > off + boxSize) break;
					std::string propType((const char*)&m_data[ipcoOff + 4], 4);
					std::vector<uint8_t> propData(&m_data[ipcoOff], &m_data[ipcoOff + propSize]);
					properties.push_back({ propType, propData });
					ipcoOff += propSize;
				}
			} else if (boxType == "ipma") {
				uint8_t ver = m_data[off + 8];
				size_t p = off + 12;
				uint32_t entryCount = ReadU32(p);
				p += 4;
				for (uint32_t e = 0; e < entryCount && p < off + boxSize; e++) {
					uint32_t itemId = (ver < 1) ? ReadU16(p) : ReadU32(p);
					p += (ver < 1) ? 2 : 4;
					uint8_t assocCount = m_data[p++];
					for (uint8_t a = 0; a < assocCount; a++) {
						uint16_t propIndex = (m_data[off + 9] & 0x01) ? ((m_data[p] & 0x7F) << 8 | m_data[p + 1]) : (m_data[p] & 0x7F);
						p += (m_data[off + 9] & 0x01) ? 2 : 1;
						if (propIndex > 0 && propIndex < properties.size()) {
							const auto& prop = properties[propIndex];
							if (prop.first == "ispe" && prop.second.size() >= 20) {
								m_items[itemId].width = (prop.second[12] << 24) | (prop.second[13] << 16) | (prop.second[14] << 8) | prop.second[15];
								m_items[itemId].height = (prop.second[16] << 24) | (prop.second[17] << 16) | (prop.second[18] << 8) | prop.second[19];
							} else if (prop.first == "hvcC") {
								m_items[itemId].hvcC_data = prop.second;
							} else if (prop.first == "colr" && prop.second.size() >= 12) {
								std::string colrType((const char*)&prop.second[8], 4);
								if (colrType == "nclx" && prop.second.size() >= 19) {
									m_items[itemId].has_nclx = true;
									m_items[itemId].matrix_coefficients = (prop.second[16] << 8) | prop.second[17];
									m_items[itemId].full_range = (prop.second[18] & 0x80) != 0;
								} else if ((colrType == "prof" || colrType == "rICC") && prop.second.size() > 12) {
									m_items[itemId].icc_profile.assign(prop.second.begin() + 12, prop.second.end());
								}
							}
						}
					}
				}
			}
			off += boxSize;
		}
	}

	void ParseIref(size_t start, size_t length) {
		if (start + 4 > m_size) return;
		uint8_t ver = m_data[start];
		size_t off = start + 4;
		size_t end = min(start + length, m_size);
		while (off + 8 <= end) {
			uint32_t boxSize = ReadU32(off);
			if (boxSize < 8 || off + boxSize > end) break;
			std::string refType((const char*)&m_data[off + 4], 4);
			if (refType == "dimg") {
				size_t p = off + 8;
				uint32_t fromId = (ver == 0) ? ReadU16(p) : ReadU32(p);
				p += (ver == 0) ? 2 : 4;
				uint16_t refCount = ReadU16(p);
				p += 2;
				for (uint16_t r = 0; r < refCount && p < off + boxSize; r++) {
					uint32_t toId = (ver == 0) ? ReadU16(p) : ReadU32(p);
					p += (ver == 0) ? 2 : 4;
					m_grids[fromId].tileItemIds.push_back(toId);
				}
			}
			off += boxSize;
		}
	}

	uint64_t ReadVarInt(size_t off, uint8_t size) const {
		if (size == 0 || off + size > m_size) return 0;
		if (size == 1) return m_data[off];
		if (size == 2) return ReadU16(off);
		if (size == 4) return ReadU32(off);
		if (size == 8) return ReadU64(off);
		return 0;
	}
};

// Build Annex-B bitstream from hvcC + mdat slice NALUs
static bool BuildAnnexBStream(
	const std::vector<uint8_t>& hvcC,
	const uint8_t* sliceData, size_t sliceSize,
	std::vector<uint8_t>& annexB)
{
	if (hvcC.size() < 23) return false;
	annexB.clear();
	annexB.reserve(hvcC.size() + sliceSize + 64);
	const uint8_t startCode[4] = { 0, 0, 0, 1 };

	size_t p = 8 + 22; // Skip box header (8 bytes) + 22 bytes in hvcC to numOfArrays
	if (p >= hvcC.size()) return false;
	uint8_t numOfArrays = hvcC[p++];

	for (uint8_t a = 0; a < numOfArrays && p + 3 <= hvcC.size(); a++) {
		p++; // nalType
		uint16_t numNalus = (hvcC[p] << 8) | hvcC[p + 1];
		p += 2;
		for (uint16_t n = 0; n < numNalus && p + 2 <= hvcC.size(); n++) {
			uint16_t nalLen = (hvcC[p] << 8) | hvcC[p + 1];
			p += 2;
			if (p + nalLen > hvcC.size()) return false;
			annexB.insert(annexB.end(), startCode, startCode + 4);
			annexB.insert(annexB.end(), &hvcC[p], &hvcC[p + nalLen]);
			p += nalLen;
		}
	}

	size_t sliceP = 0;
	while (sliceP + 4 <= sliceSize) {
		uint32_t nalLen = (sliceData[sliceP] << 24) | (sliceData[sliceP + 1] << 16) | (sliceData[sliceP + 2] << 8) | sliceData[sliceP + 3];
		sliceP += 4;
		if (sliceP + nalLen > sliceSize) break;
		annexB.insert(annexB.end(), startCode, startCode + 4);
		annexB.insert(annexB.end(), &sliceData[sliceP], &sliceData[sliceP + nalLen]);
		sliceP += nalLen;
	}

	return !annexB.empty();
}

// Convert NV12 to BGRA for a region with OpenMP + AVX2 acceleration
static void ConvertNV12ToBgraRegion(
	const uint8_t* pNV12, uint32_t tileW, uint32_t tileH, uint32_t tileStride,
	uint8_t* pDstBGRA, uint32_t dstW, uint32_t dstH,
	uint32_t dstX, uint32_t dstY,
	const YCbCrCoefficients& coeffs, bool fullRange)
{
	const uint8_t* pY = pNV12;
	const uint8_t* pUV = pNV12 + (size_t)tileStride * tileH;
	const float r_cr = coeffs.r_cr, g_cb = coeffs.g_cb, g_cr = coeffs.g_cr, b_cb = coeffs.b_cb;

	#pragma omp parallel for schedule(static)
	for (int y = 0; y < (int)tileH; y += 2) {
		if (dstY + y >= dstH) continue;
		for (int dy = 0; dy < 2 && (y + dy) < (int)tileH; dy++) {
			int curDstY = dstY + y + dy;
			if (curDstY >= (int)dstH) continue;

			const uint8_t* lineY = pY + (size_t)(y + dy) * tileStride;
			const uint8_t* lineUV = pUV + (size_t)(y / 2) * tileStride;
			uint8_t* out = pDstBGRA + (size_t)curDstY * (dstW * 4) + (size_t)dstX * 4;

			int copyW = min((int)tileW, (int)dstW - (int)dstX);
			int x = 0;

#if defined(__AVX2__)
			const __m128i maskU = _mm_setr_epi8(0, -1, 0, -1, 2, -1, 2, -1, 4, -1, 4, -1, 6, -1, 6, -1);
			const __m128i maskV = _mm_setr_epi8(1, -1, 1, -1, 3, -1, 3, -1, 5, -1, 5, -1, 7, -1, 7, -1);
			const __m128i maskU_hi = _mm_setr_epi8(8, -1, 8, -1, 10, -1, 10, -1, 12, -1, 12, -1, 14, -1, 14, -1);
			const __m128i maskV_hi = _mm_setr_epi8(9, -1, 9, -1, 11, -1, 11, -1, 13, -1, 13, -1, 15, -1, 15, -1);
			const __m256 v128_f = _mm256_set1_ps(128.0f);
			const __m256 vR_cr_f = _mm256_set1_ps(r_cr);
			const __m256 vG_cb_f = _mm256_set1_ps(g_cb);
			const __m256 vG_cr_f = _mm256_set1_ps(g_cr);
			const __m256 vB_cb_f = _mm256_set1_ps(b_cb);
			const __m256 vRangeY_scale = _mm256_set1_ps(fullRange ? 1.0f : 1.1689f);
			const __m256 vRangeY_off = _mm256_set1_ps(fullRange ? 0.0f : 16.0f);
			const __m256 vRangeUV_scale = _mm256_set1_ps(fullRange ? 1.0f : 1.1429f);

			for (; x + 15 < copyW; x += 16) {
				__m128i y_raw = _mm_loadu_si128((const __m128i*)(lineY + x));
				__m128i uv_raw = _mm_loadu_si128((const __m128i*)(lineUV + x));

				__m128i u_lo16 = _mm_shuffle_epi8(uv_raw, maskU);
				__m128i v_lo16 = _mm_shuffle_epi8(uv_raw, maskV);
				__m128i u_hi16 = _mm_shuffle_epi8(uv_raw, maskU_hi);
				__m128i v_hi16 = _mm_shuffle_epi8(uv_raw, maskV_hi);

				// Process first 8 pixels
				__m256 y8_0 = _mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(y_raw));
				__m256 u8_0 = _mm256_cvtepi32_ps(_mm256_cvtepu16_epi32(u_lo16));
				__m256 v8_0 = _mm256_cvtepi32_ps(_mm256_cvtepu16_epi32(v_lo16));

				__m256 y_val0 = _mm256_mul_ps(_mm256_sub_ps(y8_0, vRangeY_off), vRangeY_scale);
				__m256 cb0 = _mm256_mul_ps(_mm256_sub_ps(u8_0, v128_f), vRangeUV_scale);
				__m256 cr0 = _mm256_mul_ps(_mm256_sub_ps(v8_0, v128_f), vRangeUV_scale);

				__m256 r0 = _mm256_add_ps(y_val0, _mm256_mul_ps(cr0, vR_cr_f));
				__m256 g0 = _mm256_add_ps(y_val0, _mm256_add_ps(_mm256_mul_ps(cb0, vG_cb_f), _mm256_mul_ps(cr0, vG_cr_f)));
				__m256 b0 = _mm256_add_ps(y_val0, _mm256_mul_ps(cb0, vB_cb_f));

				__m256i r0_i = _mm256_cvtps_epi32(_mm256_round_ps(r0, _MM_FROUND_TO_NEAREST_INT |_MM_FROUND_NO_EXC));
				__m256i g0_i = _mm256_cvtps_epi32(_mm256_round_ps(g0, _MM_FROUND_TO_NEAREST_INT |_MM_FROUND_NO_EXC));
				__m256i b0_i = _mm256_cvtps_epi32(_mm256_round_ps(b0, _MM_FROUND_TO_NEAREST_INT |_MM_FROUND_NO_EXC));

				// Process second 8 pixels
				__m128i y_hi8 = _mm_srli_si128(y_raw, 8);
				__m256 y8_1 = _mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(y_hi8));
				__m256 u8_1 = _mm256_cvtepi32_ps(_mm256_cvtepu16_epi32(u_hi16));
				__m256 v8_1 = _mm256_cvtepi32_ps(_mm256_cvtepu16_epi32(v_hi16));

				__m256 y_val1 = _mm256_mul_ps(_mm256_sub_ps(y8_1, vRangeY_off), vRangeY_scale);
				__m256 cb1 = _mm256_mul_ps(_mm256_sub_ps(u8_1, v128_f), vRangeUV_scale);
				__m256 cr1 = _mm256_mul_ps(_mm256_sub_ps(v8_1, v128_f), vRangeUV_scale);

				__m256 r1 = _mm256_add_ps(y_val1, _mm256_mul_ps(cr1, vR_cr_f));
				__m256 g1 = _mm256_add_ps(y_val1, _mm256_add_ps(_mm256_mul_ps(cb1, vG_cb_f), _mm256_mul_ps(cr1, vG_cr_f)));
				__m256 b1 = _mm256_add_ps(y_val1, _mm256_mul_ps(cb1, vB_cb_f));

				__m256i r1_i = _mm256_cvtps_epi32(_mm256_round_ps(r1, _MM_FROUND_TO_NEAREST_INT |_MM_FROUND_NO_EXC));
				__m256i g1_i = _mm256_cvtps_epi32(_mm256_round_ps(g1, _MM_FROUND_TO_NEAREST_INT |_MM_FROUND_NO_EXC));
				__m256i b1_i = _mm256_cvtps_epi32(_mm256_round_ps(b1, _MM_FROUND_TO_NEAREST_INT |_MM_FROUND_NO_EXC));

				// Pack and store 16 pixels
				alignas(32) int r_arr[16], g_arr[16], b_arr[16];
				_mm256_store_si256((__m256i*)&r_arr[0], r0_i);
				_mm256_store_si256((__m256i*)&g_arr[0], g0_i);
				_mm256_store_si256((__m256i*)&b_arr[0], b0_i);
				_mm256_store_si256((__m256i*)&r_arr[8], r1_i);
				_mm256_store_si256((__m256i*)&g_arr[8], g1_i);
				_mm256_store_si256((__m256i*)&b_arr[8], b1_i);

				for (int k = 0; k < 16; k++) {
					int rv = r_arr[k], gv = g_arr[k], bv = b_arr[k];
					out[(x + k) * 4 + 0] = (uint8_t)(bv < 0 ? 0 : (bv > 255 ? 255 : bv));
					out[(x + k) * 4 + 1] = (uint8_t)(gv < 0 ? 0 : (gv > 255 ? 255 : gv));
					out[(x + k) * 4 + 2] = (uint8_t)(rv < 0 ? 0 : (rv > 255 ? 255 : rv));
					out[(x + k) * 4 + 3] = 0xFF;
				}
			}
#endif

			for (; x < copyW; x += 2) {
				float cb = (float)lineUV[x] - 128.0f;
				float cr = (float)lineUV[x + 1] - 128.0f;

				if (!fullRange) {
					cb *= 1.1429f;
					cr *= 1.1429f;
				}

				float r_off = r_cr * cr;
				float g_off = g_cb * cb + g_cr * cr;
				float b_off = b_cb * cb;

				for (int dx = 0; dx < 2 && (x + dx) < copyW; dx++) {
					float yval = (float)lineY[x + dx];
					if (!fullRange) yval = (yval - 16.0f) * 1.1689f;

					int b = (int)(yval + b_off + 0.5f);
					int g = (int)(yval + g_off + 0.5f);
					int r = (int)(yval + r_off + 0.5f);

					out[(x + dx) * 4 + 0] = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));
					out[(x + dx) * 4 + 1] = (uint8_t)(g < 0 ? 0 : (g > 255 ? 255 : g));
					out[(x + dx) * 4 + 2] = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
					out[(x + dx) * 4 + 3] = 0xFF;
				}
			}
		}
	}
}

} // anonymous namespace

bool GpuHeifDecoder::IsHardwareSupported()
{
	return MfGpuContext::Instance().EnsureInit();
}

bool GpuHeifDecoder::DecodeHeif(
	const void* buffer,
	size_t sizeBytes,
	int frameIndex,
	int& width,
	int& height,
	int& bpp,
	int& frameCount,
	void*& pPixelData,
	void*& exif_chunk,
	bool& hasAlpha,
	bool& outOfMemory)
{
	outOfMemory = false;
	hasAlpha = false;
	pPixelData = nullptr;
	exif_chunk = nullptr;
	width = height = 0;
	bpp = 4;
	frameCount = 1;

	if (!buffer || sizeBytes < 32) return false;
	if (!IsHardwareSupported()) return false;

	const uint8_t* data = (const uint8_t*)buffer;
	IsoHeifDemuxer demuxer;
	if (!demuxer.Parse(data, sizeBytes)) {
		return false;
	}

	frameCount = (int)demuxer.m_topLevelItemIds.size();
	if (frameCount <= 0) return false;
	if (frameIndex < 0 || frameIndex >= frameCount) {
		frameIndex = 0;
	}

	uint32_t targetItemId = demuxer.m_topLevelItemIds[frameIndex];
	const auto& targetItem = demuxer.m_items[targetItemId];

	uint32_t finalW = targetItem.width;
	uint32_t finalH = targetItem.height;

	// Check if this is a grid derived item
	bool isGrid = (demuxer.m_grids.count(targetItemId) != 0);
	if (isGrid) {
		const auto& grid = demuxer.m_grids[targetItemId];
		if (grid.tileItemIds.empty()) return false;

		// Read ImageGrid header from item offset: version(1B), flags(1B), rows_minus_one(1B), cols_minus_one(1B), output_width, output_height
		if (targetItem.offset + 8 > sizeBytes) return false;
		uint8_t version = data[targetItem.offset];
		uint8_t flags = data[targetItem.offset + 1];
		uint8_t rows = data[targetItem.offset + 2] + 1;
		uint8_t cols = data[targetItem.offset + 3] + 1;

		if (flags & 1) { // 32-bit width/height
			if (targetItem.offset + 12 > sizeBytes) return false;
			finalW = (data[targetItem.offset + 4] << 24) | (data[targetItem.offset + 5] << 16) | (data[targetItem.offset + 6] << 8) | data[targetItem.offset + 7];
			finalH = (data[targetItem.offset + 8] << 24) | (data[targetItem.offset + 9] << 16) | (data[targetItem.offset + 10] << 8) | data[targetItem.offset + 11];
		} else { // 16-bit width/height
			finalW = (data[targetItem.offset + 4] << 8) | data[targetItem.offset + 5];
			finalH = (data[targetItem.offset + 6] << 8) | data[targetItem.offset + 7];
		}

		if (finalW > MAX_IMAGE_DIMENSION || finalH > MAX_IMAGE_DIMENSION || (double)finalW * finalH > MAX_IMAGE_PIXELS || finalW < 1 || finalH < 1) {
			outOfMemory = true;
			return false;
		}

		uint8_t* pDstPixels = new(std::nothrow) uint8_t[(size_t)finalW * finalH * 4];
		if (!pDstPixels) {
			outOfMemory = true;
			return false;
		}

		// Decode each tile
		for (size_t i = 0; i < grid.tileItemIds.size(); i++) {
			uint32_t tileId = grid.tileItemIds[i];
			const auto& tileItem = demuxer.m_items[tileId];
			if (tileItem.offset + tileItem.length > sizeBytes) {
				delete[] pDstPixels;
				return false;
			}

			std::vector<uint8_t> annexB;
			if (!BuildAnnexBStream(tileItem.hvcC_data, &data[tileItem.offset], (size_t)tileItem.length, annexB)) {
				delete[] pDstPixels;
				return false;
			}

			std::vector<uint8_t> nv12;
			uint32_t stride = 0;
			if (!MfGpuContext::Instance().DecodeHevcBitstream(annexB.data(), annexB.size(), tileItem.width, tileItem.height, nv12, stride)) {
				delete[] pDstPixels;
				return false;
			}

			uint32_t tileX = (uint32_t)(i % cols) * tileItem.width;
			uint32_t tileY = (uint32_t)(i / cols) * tileItem.height;
			YCbCrCoefficients coeffs = GetNclxCoefficients(tileItem.matrix_coefficients);
			ConvertNV12ToBgraRegion(nv12.data(), tileItem.width, tileItem.height, stride, pDstPixels, finalW, finalH, tileX, tileY, coeffs, tileItem.full_range);
		}

		// Apply ICC profile if attached
		if (!targetItem.icc_profile.empty()) {
			void* transform = ICCProfileTransform::CreateTransform(targetItem.icc_profile.data(), (unsigned int)targetItem.icc_profile.size(), ICCProfileTransform::FORMAT_BGRA);
			if (transform) {
				ICCProfileTransform::DoTransform(transform, pDstPixels, pDstPixels, finalW, finalH);
				ICCProfileTransform::DeleteTransform(transform);
			}
		}

		width = (int)finalW;
		height = (int)finalH;
		pPixelData = pDstPixels;
	}
	else {
		// Single HEVC item
		if (targetItem.type != "hvc1" && targetItem.type != "hev1") {
			return false;
		}

		if (finalW > MAX_IMAGE_DIMENSION || finalH > MAX_IMAGE_DIMENSION || (double)finalW * finalH > MAX_IMAGE_PIXELS || finalW < 1 || finalH < 1) {
			outOfMemory = true;
			return false;
		}

		if (targetItem.offset + targetItem.length > sizeBytes) {
			return false;
		}

		std::vector<uint8_t> annexB;
		if (!BuildAnnexBStream(targetItem.hvcC_data, &data[targetItem.offset], (size_t)targetItem.length, annexB)) {
			return false;
		}

		std::vector<uint8_t> nv12;
		uint32_t stride = 0;
		if (!MfGpuContext::Instance().DecodeHevcBitstream(annexB.data(), annexB.size(), finalW, finalH, nv12, stride)) {
			return false;
		}

		uint8_t* pDstPixels = new(std::nothrow) uint8_t[(size_t)finalW * finalH * 4];
		if (!pDstPixels) {
			outOfMemory = true;
			return false;
		}

		YCbCrCoefficients coeffs = GetNclxCoefficients(targetItem.matrix_coefficients);
		ConvertNV12ToBgraRegion(nv12.data(), finalW, finalH, stride, pDstPixels, finalW, finalH, 0, 0, coeffs, targetItem.full_range);

		// Apply ICC profile if attached
		if (!targetItem.icc_profile.empty()) {
			void* transform = ICCProfileTransform::CreateTransform(targetItem.icc_profile.data(), (unsigned int)targetItem.icc_profile.size(), ICCProfileTransform::FORMAT_BGRA);
			if (transform) {
				ICCProfileTransform::DoTransform(transform, pDstPixels, pDstPixels, finalW, finalH);
				ICCProfileTransform::DeleteTransform(transform);
			}
		}

		width = (int)finalW;
		height = (int)finalH;
		pPixelData = pDstPixels;
	}

	// Extract Exif metadata if present
	if (demuxer.m_exifItemId != 0) {
		const auto& exifItem = demuxer.m_items[demuxer.m_exifItemId];
		if (exifItem.offset + exifItem.length <= sizeBytes && exifItem.length > 4) {
			// In ISOBMFF, Exif item starts with a 4-byte offset indicating start of TIFF header
			uint32_t exifOffset = (data[exifItem.offset] << 24) | (data[exifItem.offset + 1] << 16) | (data[exifItem.offset + 2] << 8) | data[exifItem.offset + 3];
			size_t actualExifStart = exifItem.offset + 4 + exifOffset;
			if (actualExifStart < exifItem.offset + exifItem.length) {
				size_t exifPayloadSize = (exifItem.offset + exifItem.length) - actualExifStart;
				if (exifPayloadSize > 4 && exifPayloadSize < 65530) {
					// Format as JPEG Exif chunk: FF E1 [2-byte size] [payload]
					size_t chunkSize = exifPayloadSize + 4;
					exif_chunk = malloc(chunkSize);
					if (exif_chunk) {
						uint8_t* pExif = (uint8_t*)exif_chunk;
						pExif[0] = 0xFF;
						pExif[1] = 0xE1;
						pExif[2] = (uint8_t)((chunkSize - 2) >> 8);
						pExif[3] = (uint8_t)((chunkSize - 2) & 0xFF);
						memcpy(pExif + 4, &data[actualExifStart], exifPayloadSize);
					}
				}
			}
		}
	}

	return true;
}
