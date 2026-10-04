#include "stdafx.h"
#include "resource.h"
#include "OcrController.h"
#include "MainDlg.h"
#include "JPEGImage.h"
#include "HelpersGUI.h"
#include "NLS.h"
#include <mutex>
#include <thread>
#include <cmath>

struct COcrController::Job {
    std::mutex mutex;
    std::atomic<bool> canceled{false};
    bool done = false;
    HWND window = nullptr;
    Ocr::Result result;
};

COcrController::COcrController() = default;
COcrController::~COcrController() { Shutdown(); }

bool COcrController::Busy() const {
    if (!m_job) return false;
    std::lock_guard lock(m_job->mutex);
    return !m_job->done && !m_job->canceled.load();
}

void COcrController::Reset() {
    if (m_job) m_job->canceled.store(true);
    m_result = {};
    m_active = m_selecting = false;
    m_anchor = m_end = -1;
}

void COcrController::Shutdown() {
    Reset();
    if (m_job) {
        std::lock_guard lock(m_job->mutex);
        m_job->window = nullptr;
    }
}

void COcrController::Toggle(CMainDlg& main) {
    if (m_active) {
        m_active = m_selecting = false;
        main.Invalidate(FALSE);
        return;
    }
    if (!m_result.words.empty()) {
        m_active = true;
        main.Invalidate(FALSE);
        return;
    }
    if (m_job) {
        std::lock_guard lock(m_job->mutex);
        if (!m_job->done) return; // one bounded job; repeated clicks never queue work
    }
    auto image = main.GetCurrentImage();
    if (!image) return;
    try {
        // OriginalPixels applies any deferred EXIF/display rotation before sizing.
        const void* pixels = image->OriginalPixels();
        auto snapshot = Ocr::MakeSnapshot(pixels, image->OrigWidth(), image->OrigHeight(), image->OriginalChannels());
        auto job = std::make_shared<Job>();
        job->window = main.GetHWND();
        m_job = job;
        m_result = {};
        m_anchor = m_end = -1;
        std::thread([job, snapshot = std::move(snapshot)]() {
            auto result = Ocr::Recognize(snapshot, job->canceled);
            std::lock_guard lock(job->mutex);
            job->result = std::move(result);
            job->done = true;
            // No owning pointers in the message queue, and no access to CMainDlg.
            if (job->window) ::PostMessage(job->window, WM_OCR_COMPLETED, 0, 0);
        }).detach();
    } catch (const std::exception&) {
        if (m_job) { std::lock_guard lock(m_job->mutex); m_job->done = true; }
        ::MessageBox(main.GetHWND(), CNLS::GetString(_T("Could not start text recognition. Memory may be low.")), _T("OCR"), MB_OK | MB_ICONERROR);
    }
    main.Invalidate(FALSE);
}

void COcrController::Complete(CMainDlg& main) {
    if (!m_job) return;
    {
        std::lock_guard lock(m_job->mutex);
        if (!m_job->done) return;
        if (!m_job->canceled.load()) m_result = std::move(m_job->result);
    }
    m_job.reset();
    m_active = !m_result.words.empty();
    main.Invalidate(FALSE);
    if (!m_result.error.empty()) {
        ::MessageBox(main.GetHWND(), m_result.error.c_str(), _T("OCR"), MB_OK | MB_ICONINFORMATION);
    } else if (!m_active && main.GetCurrentImage() && !m_result.language.empty()) {
        ::MessageBox(main.GetHWND(), CNLS::GetString(_T("No text found in this image.")), _T("OCR"), MB_OK | MB_ICONINFORMATION);
    }
}

int COcrController::Hit(CMainDlg& main, CPoint point) const {
    float x = float(point.x - main.GetDIBOffset().x), y = float(point.y - main.GetDIBOffset().y);
    if (!main.ScreenToImage(x, y)) return -1;
    for (size_t i = 0; i < m_result.words.size(); ++i)
        if (Ocr::Contains(m_result.words[i], {x, y})) return int(i);
    return -1;
}

bool COcrController::MouseDown(CMainDlg& main, CPoint point) {
    if (!m_active) return false;
    const int hit = Hit(main, point);
    if (hit < 0) {
        m_anchor = m_end = -1;
        main.Invalidate(FALSE);
        return false; // background remains available for panning
    }
    m_anchor = m_end = hit;
    m_selecting = true;
    main.Invalidate(FALSE);
    return true;
}

bool COcrController::MouseMove(CMainDlg& main, CPoint point) {
    if (!m_active) return false;
    const int hit = Hit(main, point);
    if (m_selecting) {
        if (hit >= 0 && m_end != hit) { m_end = hit; main.Invalidate(FALSE); }
    }
    if (hit >= 0 || m_selecting) { ::SetCursor(::LoadCursor(nullptr, IDC_IBEAM)); return true; }
    return false;
}

bool COcrController::MouseUp() {
    const bool selecting = m_selecting;
    m_selecting = false;
    return selecting;
}

void COcrController::SelectAll() {
    if (!m_active || m_result.words.empty()) return;
    m_anchor = 0;
    m_end = int(m_result.words.size()) - 1;
}

bool COcrController::Copy(HWND window) {
    const auto text = Ocr::SelectedText(m_result.words, m_anchor, m_end);
    if (text.empty()) return false;
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) return true;
    auto data = ::GlobalLock(memory);
    if (!data) { ::GlobalFree(memory); return true; }
    memcpy(data, text.c_str(), bytes);
    ::GlobalUnlock(memory);
    if (::OpenClipboard(window)) {
        if (::EmptyClipboard() && ::SetClipboardData(CF_UNICODETEXT, memory)) memory = nullptr;
        ::CloseClipboard();
    }
    if (memory) {
        ::GlobalFree(memory);
        ::MessageBox(window, CNLS::GetString(_T("Could not copy text. The clipboard may be busy.")), _T("OCR"), MB_OK | MB_ICONINFORMATION);
    }
    return true;
}

void COcrController::Paint(CMainDlg& main, HDC dc) {
    if (m_active) {
        const int saved = ::SaveDC(dc);
        HPEN outline = ::CreatePen(PS_SOLID, 1, RGB(100, 210, 245));
        HPEN selected = ::CreatePen(PS_SOLID, 3, RGB(255, 205, 65));
        ::SelectObject(dc, ::GetStockObject(HOLLOW_BRUSH));
        const int first = min(m_anchor, m_end), last = max(m_anchor, m_end);
        const CPoint offset = main.GetDIBOffset();
        for (size_t i = 0; i < m_result.words.size(); ++i) {
            POINT polygon[5];
            for (size_t j = 0; j < 4; ++j) {
                float x = m_result.words[i].corners[j].x, y = m_result.words[i].corners[j].y;
                main.ImageToScreen(x, y);
                polygon[j] = {LONG(std::lround(x)) + offset.x, LONG(std::lround(y)) + offset.y};
            }
            polygon[4] = polygon[0];
            ::SelectObject(dc, first >= 0 && int(i) >= first && int(i) <= last ? selected : outline);
            ::Polyline(dc, polygon, 5);
        }
        ::RestoreDC(dc, saved);
        ::DeleteObject(outline);
        ::DeleteObject(selected);
    }
    const bool busy = Busy();
    if (busy || m_active) {
        const int saved = ::SaveDC(dc);
        ::SetTextColor(dc, RGB(255, 255, 255));
        ::SetBkColor(dc, RGB(30, 35, 40));
        ::SetBkMode(dc, OPAQUE);
        const wchar_t* text = busy ? L"Recognizing text..." : L"OCR: drag across words | Ctrl+A select all | Ctrl+C copy | Esc hide";
        ::TextOutW(dc, HelpersGUI::ScaleToScreen(12), HelpersGUI::ScaleToScreen(36), text, int(wcslen(text)));
        ::RestoreDC(dc, saved);
    }
}
