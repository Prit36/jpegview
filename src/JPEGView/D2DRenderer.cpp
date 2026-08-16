#include "StdAfx.h"
#include "D2DRenderer.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

CD2DRenderer::CD2DRenderer() = default;
CD2DRenderer::~CD2DRenderer() {
    Cleanup();
}

CD2DRenderer& CD2DRenderer::This() {
    static CD2DRenderer instance;
    return instance;
}

bool CD2DRenderer::Initialize(HWND hWnd) {
    m_hWnd = hWnd;
    if (!CreateDeviceResources()) {
        return false;
    }

    RECT rc;
    ::GetClientRect(hWnd, &rc);
    m_width = (std::max)(1L, (long)(rc.right - rc.left));
    m_height = (std::max)(1L, (long)(rc.bottom - rc.top));

    if (!CreateWindowSizeDependentResources()) {
        return false;
    }

    UpdateDisplayCapabilities();
    m_initialized = true;
    return true;
}

void CD2DRenderer::Cleanup() {
    m_initialized = false;
    m_solidBrush.Reset();
    m_d2dTargetBitmap.Reset();
    m_d2dContext.Reset();
    m_d2dDevice.Reset();
    m_d2dFactory.Reset();
    m_dwriteFactory.Reset();
    m_swapChain.Reset();
    m_d3dContext.Reset();
    m_d3dDevice.Reset();
}

bool CD2DRenderer::CreateDeviceResources() {
    // 1. Create D2D Factory
    D2D1_FACTORY_OPTIONS options = {};
#if defined(_DEBUG)
    options.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory7), &options, &m_d2dFactory);
    if (FAILED(hr)) return false;

    // 2. Create DirectWrite Factory
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory7), &m_dwriteFactory);
    if (FAILED(hr)) return false;

    // 3. Create Direct3D 11 Device
    UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    creationFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };

    ComPtr<ID3D11Device> baseDevice;
    ComPtr<ID3D11DeviceContext> baseContext;
    D3D_FEATURE_LEVEL featureLevel;

    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, creationFlags,
                           featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION,
                           &baseDevice, &featureLevel, &baseContext);
    if (FAILED(hr)) return false;

    hr = baseDevice.As(&m_d3dDevice);
    if (FAILED(hr)) return false;
    hr = baseContext.As(&m_d3dContext);
    if (FAILED(hr)) return false;

    // 4. Create D2D Device & DeviceContext
    ComPtr<IDXGIDevice> dxgiDevice;
    hr = m_d3dDevice.As(&dxgiDevice);
    if (FAILED(hr)) return false;

    hr = m_d2dFactory->CreateDevice(dxgiDevice.Get(), &m_d2dDevice);
    if (FAILED(hr)) return false;

    hr = m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_ENABLE_MULTITHREADED_OPTIMIZATIONS, &m_d2dContext);
    if (FAILED(hr)) return false;

    return true;
}

bool CD2DRenderer::CreateWindowSizeDependentResources() {
    if (!m_d3dDevice || !m_hWnd) return false;

    m_d2dContext->SetTarget(nullptr);
    m_d2dTargetBitmap.Reset();

    if (m_swapChain) {
        // Resize existing swapchain
        HRESULT hr = m_swapChain->ResizeBuffers(2, m_width, m_height, DXGI_FORMAT_R16G16B16A16_FLOAT, 0);
        if (FAILED(hr)) {
            m_swapChain.Reset();
        }
    }

    if (!m_swapChain) {
        // Create DXGI SwapChain
        ComPtr<IDXGIDevice> dxgiDevice;
        if (FAILED(m_d3dDevice.As(&dxgiDevice))) return false;

        ComPtr<IDXGIAdapter> dxgiAdapter;
        if (FAILED(dxgiDevice->GetAdapter(&dxgiAdapter))) return false;

        ComPtr<IDXGIFactory5> dxgiFactory;
        if (FAILED(dxgiAdapter->GetParent(IID_PPV_ARGS(&dxgiFactory)))) return false;

        DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
        swapChainDesc.Width = m_width;
        swapChainDesc.Height = m_height;
        swapChainDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; // FP16 for native HDR/scRGB
        swapChainDesc.Stereo = FALSE;
        swapChainDesc.SampleDesc.Count = 1;
        swapChainDesc.SampleDesc.Quality = 0;
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDesc.BufferCount = 2;
        swapChainDesc.Scaling = DXGI_SCALING_NONE;
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

        ComPtr<IDXGISwapChain1> swapChain1;
        HRESULT hr = dxgiFactory->CreateSwapChainForHwnd(m_d3dDevice.Get(), m_hWnd, &swapChainDesc, nullptr, nullptr, &swapChain1);
        if (FAILED(hr)) return false;

        hr = swapChain1.As(&m_swapChain);
        if (FAILED(hr)) return false;
    }

    // Set Color Space for HDR / scRGB
    if (m_swapChain) {
        m_swapChain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
    }

    // Create D2D Target Bitmap from SwapChain backbuffer
    ComPtr<IDXGISurface2> dxgiBackBuffer;
    HRESULT hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&dxgiBackBuffer));
    if (FAILED(hr)) return false;

    D2D1_BITMAP_PROPERTIES1 bitmapProperties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f
    );

    hr = m_d2dContext->CreateBitmapFromDxgiSurface(dxgiBackBuffer.Get(), &bitmapProperties, &m_d2dTargetBitmap);
    if (FAILED(hr)) return false;

    m_d2dContext->SetTarget(m_d2dTargetBitmap.Get());

    if (FAILED(m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &m_solidBrush))) {
        return false;
    }

    return true;
}

void CD2DRenderer::Resize(UINT width, UINT height) {
    if (width == 0 || height == 0) return;
    if (m_width == width && m_height == height) return;

    m_width = width;
    m_height = height;
    CreateWindowSizeDependentResources();
}

void CD2DRenderer::UpdateDisplayCapabilities() {
    if (!m_swapChain) return;

    ComPtr<IDXGIOutput> output;
    if (FAILED(m_swapChain->GetContainingOutput(&output))) return;

    ComPtr<IDXGIOutput6> output6;
    if (SUCCEEDED(output.As(&output6))) {
        DXGI_OUTPUT_DESC1 desc1;
        if (SUCCEEDED(output6->GetDesc1(&desc1))) {
            m_hdrInfo.IsHDREnabled = (desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
                                      desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
            m_hdrInfo.MaxLuminance = desc1.MaxLuminance;
            m_hdrInfo.MinLuminance = desc1.MinLuminance;
            m_hdrInfo.MaxFullFrameLuminance = desc1.MaxFullFrameLuminance;
            m_hdrInfo.ColorSpace = desc1.ColorSpace;
            m_hdrInfo.BitsPerPixel = (float)desc1.BitsPerColor;
        }
    }
}

bool CD2DRenderer::BeginDraw() {
    if (!m_d2dContext) return false;
    m_d2dContext->BeginDraw();
    return true;
}

void CD2DRenderer::EndDraw() {
    if (!m_d2dContext) return;
    m_d2dContext->EndDraw();
}

void CD2DRenderer::Clear(D2D1_COLOR_F color) {
    if (m_d2dContext) {
        m_d2dContext->Clear(color);
    }
}

void CD2DRenderer::DrawImage(ID2D1Bitmap1* pBitmap, const D2D1_RECT_F& destRect, const D2D1_RECT_F& srcRect,
                             D2D1_INTERPOLATION_MODE interpolationMode, float opacity) {
    if (!m_d2dContext || !pBitmap) return;

    m_d2dContext->DrawBitmap(pBitmap, destRect, opacity, interpolationMode, &srcRect);
}

void CD2DRenderer::DrawTextW(const std::wstring& text, const D2D1_RECT_F& layoutRect, float fontSize, D2D1_COLOR_F color) {
    if (!m_d2dContext || !m_dwriteFactory || !m_solidBrush) return;

    ComPtr<IDWriteTextFormat> textFormat;
    HRESULT hr = m_dwriteFactory->CreateTextFormat(
        L"Segoe UI",
        nullptr,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        fontSize,
        L"en-us",
        &textFormat
    );

    if (SUCCEEDED(hr)) {
        m_solidBrush->SetColor(color);
        m_d2dContext->DrawText(text.c_str(), (UINT32)text.length(), textFormat.Get(), layoutRect, m_solidBrush.Get());
    }
}

ComPtr<ID2D1Bitmap1> CD2DRenderer::CreateBitmapFromBGRA(int width, int height, const void* pPixels, int stride) {
    if (!m_d2dContext || width <= 0 || height <= 0 || !pPixels) return nullptr;

    if (stride <= 0) {
        stride = width * 4;
    }

    D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f
    );

    ComPtr<ID2D1Bitmap1> bitmap;
    D2D1_SIZE_U size = D2D1::SizeU(width, height);
    HRESULT hr = m_d2dContext->CreateBitmap(size, pPixels, stride, &properties, &bitmap);
    if (FAILED(hr)) return nullptr;

    return bitmap;
}

ComPtr<ID2D1Bitmap1> CD2DRenderer::CreateBitmapFromFP16(int width, int height, const void* pPixels, int stride) {
    if (!m_d2dContext || width <= 0 || height <= 0 || !pPixels) return nullptr;

    if (stride <= 0) {
        stride = width * 8; // 4 channels * 2 bytes (FP16)
    }

    D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f
    );

    ComPtr<ID2D1Bitmap1> bitmap;
    D2D1_SIZE_U size = D2D1::SizeU(width, height);
    HRESULT hr = m_d2dContext->CreateBitmap(size, pPixels, stride, &properties, &bitmap);
    if (FAILED(hr)) return nullptr;

    return bitmap;
}

HRESULT CD2DRenderer::Present(bool vsync) {
    if (!m_swapChain) return E_FAIL;
    return m_swapChain->Present(vsync ? 1 : 0, 0);
}
