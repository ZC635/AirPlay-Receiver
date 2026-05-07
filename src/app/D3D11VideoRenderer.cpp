#include "app/D3D11VideoRenderer.h"

#include "app/VideoRenderGeometry.h"

#include <QRectF>
#include <QSizeF>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include <algorithm>
#include <cstring>

namespace {
template <typename T>
void releaseCom(T *&ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

struct Vertex {
    float position[3];
    float texcoord[2];
};

const char shaderSource[] = R"(
struct VSInput {
    float3 position : POSITION;
    float2 texcoord : TEXCOORD0;
};

struct PSInput {
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD0;
};

PSInput vsMain(VSInput input) {
    PSInput output;
    output.position = float4(input.position, 1.0f);
    output.texcoord = input.texcoord;
    return output;
}

Texture2D frameTexture : register(t0);
SamplerState frameSampler : register(s0);

float4 psMain(PSInput input) : SV_TARGET {
    return frameTexture.Sample(frameSampler, input.texcoord);
}
)";
}

D3D11VideoRenderer::D3D11VideoRenderer() = default;

D3D11VideoRenderer::~D3D11VideoRenderer() {
    releaseAll();
}

bool D3D11VideoRenderer::initialize(HWND hwnd) {
    releaseAll();

    if (!hwnd || !IsWindow(hwnd)) {
        return false;
    }

    RECT rect{};
    if (!GetClientRect(hwnd, &rect)) {
        return false;
    }

    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) {
        return false;
    }

    DXGI_SWAP_CHAIN_DESC swapChainDesc{};
    swapChainDesc.BufferDesc.Width = static_cast<UINT>(width);
    swapChainDesc.BufferDesc.Height = static_cast<UINT>(height);
    swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount = 2;
    swapChainDesc.OutputWindow = hwnd;
    swapChainDesc.Windowed = TRUE;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    constexpr D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL featureLevel{};

    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                               featureLevels, std::size(featureLevels),
                                               D3D11_SDK_VERSION, &swapChainDesc, &m_swapChain,
                                               &m_device, &featureLevel, &m_context);
    if (FAILED(hr)) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                                           featureLevels, std::size(featureLevels),
                                           D3D11_SDK_VERSION, &swapChainDesc, &m_swapChain,
                                           &m_device, &featureLevel, &m_context);
    }
    if (FAILED(hr)) {
        releaseAll();
        return false;
    }

    m_hwnd = hwnd;
    m_width = width;
    m_height = height;

    if (!createRenderTarget() || !createShaders() || !createSampler() || !createVertexBuffer()) {
        releaseAll();
        return false;
    }

    return true;
}

bool D3D11VideoRenderer::isInitialized() const {
    return m_device && m_context && m_swapChain && m_renderTargetView && m_vertexShader &&
           m_pixelShader && m_inputLayout && m_vertexBuffer && m_sampler;
}

bool D3D11VideoRenderer::resize(int width, int height) {
    if (!isInitialized() || width <= 0 || height <= 0) {
        return false;
    }
    if (width == m_width && height == m_height) {
        return true;
    }

    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    m_context->ClearState();
    m_context->Flush();
    releaseRenderTarget();
    HRESULT hr = m_swapChain->ResizeBuffers(2, static_cast<UINT>(width), static_cast<UINT>(height),
                                            DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    if (FAILED(hr)) {
        releaseAll();
        return false;
    }

    m_width = width;
    m_height = height;
    if (!createRenderTarget()) {
        releaseAll();
        return false;
    }
    return true;
}

bool D3D11VideoRenderer::uploadFrame(const QImage &frame) {
    if (!isInitialized() || frame.isNull() || frame.width() <= 0 || frame.height() <= 0) {
        return false;
    }

    const QImage rgba = frame.convertToFormat(QImage::Format_RGBA8888);
    D3D11_TEXTURE2D_DESC textureDesc{};
    textureDesc.Width = static_cast<UINT>(rgba.width());
    textureDesc.Height = static_cast<UINT>(rgba.height());
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DYNAMIC;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    textureDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    ID3D11Texture2D *texture = nullptr;
    HRESULT hr = m_device->CreateTexture2D(&textureDesc, nullptr, &texture);
    if (FAILED(hr)) {
        return false;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = m_context->Map(texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) {
        releaseCom(texture);
        return false;
    }

    for (int y = 0; y < rgba.height(); ++y) {
        std::memcpy(static_cast<unsigned char *>(mapped.pData) + y * mapped.RowPitch,
                    rgba.constScanLine(y), static_cast<size_t>(rgba.bytesPerLine()));
    }
    m_context->Unmap(texture, 0);

    ID3D11ShaderResourceView *view = nullptr;
    hr = m_device->CreateShaderResourceView(texture, nullptr, &view);
    if (FAILED(hr)) {
        releaseCom(texture);
        return false;
    }

    releaseFrame();
    m_frameTexture = texture;
    m_frameView = view;
    m_frameWidth = rgba.width();
    m_frameHeight = rgba.height();
    return true;
}

bool D3D11VideoRenderer::render(bool fit) {
    if (!isInitialized() || m_width <= 0 || m_height <= 0) {
        return false;
    }

    const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    m_context->OMSetRenderTargets(1, &m_renderTargetView, nullptr);
    m_context->ClearRenderTargetView(m_renderTargetView, white);

    if (m_frameView && m_frameWidth > 0 && m_frameHeight > 0) {
        const QRectF target = videoTargetRect(QSizeF(m_frameWidth, m_frameHeight), QSizeF(m_width, m_height), fit);
        if (!target.isEmpty()) {
            const float left = static_cast<float>((target.left() / m_width) * 2.0 - 1.0);
            const float right = static_cast<float>((target.right() / m_width) * 2.0 - 1.0);
            const float top = static_cast<float>(1.0 - (target.top() / m_height) * 2.0);
            const float bottom = static_cast<float>(1.0 - (target.bottom() / m_height) * 2.0);
            const Vertex vertices[] = {
                {{left, top, 0.0f}, {0.0f, 0.0f}},
                {{right, top, 0.0f}, {1.0f, 0.0f}},
                {{left, bottom, 0.0f}, {0.0f, 1.0f}},
                {{right, bottom, 0.0f}, {1.0f, 1.0f}},
            };

            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(m_context->Map(m_vertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                return false;
            }
            std::memcpy(mapped.pData, vertices, sizeof(vertices));
            m_context->Unmap(m_vertexBuffer, 0);

            D3D11_VIEWPORT viewport{};
            viewport.Width = static_cast<float>(m_width);
            viewport.Height = static_cast<float>(m_height);
            viewport.MinDepth = 0.0f;
            viewport.MaxDepth = 1.0f;
            m_context->RSSetViewports(1, &viewport);

            const UINT stride = sizeof(Vertex);
            const UINT offset = 0;
            m_context->IASetInputLayout(m_inputLayout);
            m_context->IASetVertexBuffers(0, 1, &m_vertexBuffer, &stride, &offset);
            m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            m_context->VSSetShader(m_vertexShader, nullptr, 0);
            m_context->PSSetShader(m_pixelShader, nullptr, 0);
            m_context->PSSetShaderResources(0, 1, &m_frameView);
            m_context->PSSetSamplers(0, 1, &m_sampler);
            m_context->Draw(4, 0);

            ID3D11ShaderResourceView *nullView = nullptr;
            m_context->PSSetShaderResources(0, 1, &nullView);
        }
    }

    return SUCCEEDED(m_swapChain->Present(1, 0));
}

void D3D11VideoRenderer::resetFrame() {
    releaseFrame();
}

void D3D11VideoRenderer::releaseRenderTarget() {
    releaseCom(m_renderTargetView);
}

void D3D11VideoRenderer::releaseFrame() {
    releaseCom(m_frameView);
    releaseCom(m_frameTexture);
    m_frameWidth = 0;
    m_frameHeight = 0;
}

void D3D11VideoRenderer::releaseAll() {
    releaseFrame();
    releaseRenderTarget();
    releaseCom(m_sampler);
    releaseCom(m_vertexBuffer);
    releaseCom(m_inputLayout);
    releaseCom(m_pixelShader);
    releaseCom(m_vertexShader);
    releaseCom(m_swapChain);
    releaseCom(m_context);
    releaseCom(m_device);
    m_hwnd = nullptr;
    m_width = 0;
    m_height = 0;
}

bool D3D11VideoRenderer::createRenderTarget() {
    ID3D11Texture2D *backBuffer = nullptr;
    HRESULT hr = m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&backBuffer));
    if (FAILED(hr)) {
        return false;
    }

    hr = m_device->CreateRenderTargetView(backBuffer, nullptr, &m_renderTargetView);
    releaseCom(backBuffer);
    return SUCCEEDED(hr);
}

bool D3D11VideoRenderer::createShaders() {
    ID3DBlob *vertexBlob = nullptr;
    ID3DBlob *pixelBlob = nullptr;
    ID3DBlob *errorBlob = nullptr;

    HRESULT hr = D3DCompile(shaderSource, sizeof(shaderSource) - 1, nullptr, nullptr, nullptr,
                            "vsMain", "vs_4_0", 0, 0, &vertexBlob, &errorBlob);
    releaseCom(errorBlob);
    if (FAILED(hr)) {
        releaseCom(vertexBlob);
        return false;
    }

    hr = D3DCompile(shaderSource, sizeof(shaderSource) - 1, nullptr, nullptr, nullptr,
                    "psMain", "ps_4_0", 0, 0, &pixelBlob, &errorBlob);
    releaseCom(errorBlob);
    if (FAILED(hr)) {
        releaseCom(vertexBlob);
        releaseCom(pixelBlob);
        return false;
    }

    hr = m_device->CreateVertexShader(vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(),
                                      nullptr, &m_vertexShader);
    if (SUCCEEDED(hr)) {
        hr = m_device->CreatePixelShader(pixelBlob->GetBufferPointer(), pixelBlob->GetBufferSize(),
                                         nullptr, &m_pixelShader);
    }

    if (SUCCEEDED(hr)) {
        const D3D11_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        hr = m_device->CreateInputLayout(layout, std::size(layout), vertexBlob->GetBufferPointer(),
                                         vertexBlob->GetBufferSize(), &m_inputLayout);
    }

    releaseCom(vertexBlob);
    releaseCom(pixelBlob);
    return SUCCEEDED(hr);
}

bool D3D11VideoRenderer::createSampler() {
    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MinLOD = 0;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    return SUCCEEDED(m_device->CreateSamplerState(&samplerDesc, &m_sampler));
}

bool D3D11VideoRenderer::createVertexBuffer() {
    D3D11_BUFFER_DESC bufferDesc{};
    bufferDesc.ByteWidth = sizeof(Vertex) * 4;
    bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
    bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(m_device->CreateBuffer(&bufferDesc, nullptr, &m_vertexBuffer));
}
