#pragma once

#include <QImage>

#include <dxgi.h>
#include <windows.h>

struct ID3D11Buffer;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11InputLayout;
struct ID3D11PixelShader;
struct ID3D11RenderTargetView;
struct ID3D11SamplerState;
struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;
struct ID3D11VertexShader;
struct IDXGISwapChain;

class D3D11VideoRenderer {
public:
    D3D11VideoRenderer();
    ~D3D11VideoRenderer();

    D3D11VideoRenderer(const D3D11VideoRenderer &) = delete;
    D3D11VideoRenderer &operator=(const D3D11VideoRenderer &) = delete;

    static DXGI_SWAP_EFFECT preferredSwapEffect();
    bool initialize(HWND hwnd);
    bool isInitialized() const;
    bool resize(int width, int height);
    bool uploadFrame(const QImage &frame);
    bool render(bool fit);
    void resetFrame();

private:
    void releaseRenderTarget();
    void releaseFrame();
    void releaseAll();
    bool createRenderTarget();
    bool createShaders();
    bool createSampler();
    bool createVertexBuffer();

    HWND m_hwnd = nullptr;
    int m_width = 0;
    int m_height = 0;
    int m_frameWidth = 0;
    int m_frameHeight = 0;

    ID3D11Device *m_device = nullptr;
    ID3D11DeviceContext *m_context = nullptr;
    IDXGISwapChain *m_swapChain = nullptr;
    ID3D11RenderTargetView *m_renderTargetView = nullptr;
    ID3D11VertexShader *m_vertexShader = nullptr;
    ID3D11PixelShader *m_pixelShader = nullptr;
    ID3D11InputLayout *m_inputLayout = nullptr;
    ID3D11Buffer *m_vertexBuffer = nullptr;
    ID3D11SamplerState *m_sampler = nullptr;
    ID3D11Texture2D *m_frameTexture = nullptr;
    ID3D11ShaderResourceView *m_frameView = nullptr;
};
