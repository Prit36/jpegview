#pragma once

#include "OcrEngine.h"
#include <memory>

class CMainDlg;
class CJPEGImage;

class COcrController {
public:
    COcrController();
    ~COcrController();
    void Toggle(CMainDlg& main);
    void Reset();
    void Shutdown();
    void Complete(CMainDlg& main);
    void Paint(CMainDlg& main, HDC dc);
    bool MouseDown(CMainDlg& main, CPoint point);
    bool MouseMove(CMainDlg& main, CPoint point);
    bool MouseUp();
    bool Active() const { return m_active; }
    bool Selecting() const { return m_selecting; }
    bool Busy() const;
    void SelectAll();
    bool Copy(HWND window);
private:
    struct Job;
    std::shared_ptr<Job> m_job;
    Ocr::Result m_result;
    bool m_active = false, m_selecting = false;
    int m_anchor = -1, m_end = -1;
    int Hit(CMainDlg& main, CPoint point) const;
};
