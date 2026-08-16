#pragma once

#include <d2d1_3.h>
#include <d2d1effects_2.h>
#include <dwrite_3.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <string>

using Microsoft::WRL::ComPtr;

// Display capabilities structure for HDR/WCG
struct DisplayHDRInfo {
    bool IsHDREnabled = false;
    float MaxLuminance = 80.0f;       // Peak brightness in nits
    float MinLuminance = 0.001f;      // Black level in nits
    float MaxFullFrameLuminance = 80.0f;
    DXGI_COLOR_SPACE_TYPE ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    float BitsPerPixel = 8.0f;
};

// High-performance Direct2D 1.3 & DirectWrite rendering engine with True HDR support
class CD2DRenderer {
public:
    static CD2DRenderer& This();

    bool Initialize(HWND hWnd);
    void Cleanup();
    void Resize(UINT width, UINT height);

    // Display & HDR Detection
    void UpdateDisplayCapabilities();
    const DisplayHDRInfo& GetHDRInfo() const { return m_hdrInfo; }

    // Begin / End Draw
    bool BeginDraw();
    void EndDraw();

    // Drawing Operations
    void Clear(D2D1_COLOR_F color);
    void DrawImage(ID2D1Bitmap1* pBitmap, const D2D1_RECT_F& destRect, const D2D1_RECT_F& srcRect,
                   D2D1_INTERPOLATION_MODE interpolationMode = D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                   float opacity = 1.0f);

    void DrawTextW(const std::wstring& text, const D2D1_RECT_F& layoutRect, 
                   float fontSize = 14.0f, D2D1_COLOR_F color = D2D1::ColorF(D2D1::ColorF::White));

    // Bitmap Factory
    ComPtr<ID2D1Bitmap1> CreateBitmapFromBGRA(int width, int height, const void* pPixels, int stride = 0);
    ComPtr<ID2D1Bitmap1> CreateBitmapFromFP16(int width, int height, const void* pPixels, int stride = 0);

    // Present to screen with vsync option
    HRESULT Present(bool vsync = true);

    bool IsInitialized() const { return m_initialized; }
    ID2D1DeviceContext5* GetDeviceContext() const { return m_d2dContext.Get(); }

private:
    CD2DRenderer();
    ~CD2DRenderer();

    bool CreateDeviceResources();
    bool CreateWindowSizeDependentResources();

    HWND m_hWnd = nullptr;
    UINT m_width = 0;
    UINT m_height = 0;
    bool m_initialized = false;

    // Direct3D & DXGI
    ComPtr<ID3D11Device> m_d3dDevice;
    ComPtr<ID3D11DeviceContext> m_d3dContext;
    ComPtr<IDXGISwapChain4> m_swapChain;

    // Direct2D & DirectWrite
    ComPtr<ID2D1Factory7> m_d2dFactory;
    ComPtr<ID2D1Device5> m_d2dDevice;
    ComPtr<ID2D1DeviceContext5> m_d2dContext;
    ComPtr<ID2D1Bitmap1> m_d2dTargetBitmap;
    ComPtr<IDWriteFactory7> m_dwriteFactory;
    ComPtr<ID2D1SolidColorBrush> m_solidBrush;

    DisplayHDRInfo m_hdrInfo;
};
