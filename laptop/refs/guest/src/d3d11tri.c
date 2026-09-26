// d3d11tri.c — checkpoint D2: minimal D3D11 triangle (DXVK → Vulkan path).
// Build (mingw-w64, in the rootfs):
//   x86_64-w64-mingw32-gcc -O2 -municode -o d3d11tri.exe d3d11tri.c -ld3d11 -ldxgi -ld3dcompiler -luser32 -lgdi32
// Runs for RL_D3D_FRAMES frames (default 300) then exits 0; prints the
// adapter name and average frame time to stdout. Colour cycles so a
// screenshot at any time is distinguishable from a stale frame.
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *g_shader =
    "struct VSOut { float4 pos : SV_Position; float3 col : COLOR; };\n"
    "cbuffer CB : register(b0) { float4 tint; };\n"
    "VSOut vs(uint id : SV_VertexID) {\n"
    "  float2 p[3] = { float2(-0.7,-0.6), float2(0.0,0.7), float2(0.7,-0.6) };\n"
    "  float3 c[3] = { float3(1,0,0), float3(0,1,0), float3(0,0,1) };\n"
    "  VSOut o; o.pos = float4(p[id], 0, 1); o.col = c[id]; return o; }\n"
    "float4 ps(VSOut i) : SV_Target { return float4(i.col * tint.xyz, 1); }\n";

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

int wmain(int argc, wchar_t **argv) {
    (void)argc; (void)argv;
    int frames = 300;
    const char *env = getenv("RL_D3D_FRAMES");
    if (env) frames = atoi(env);

    HINSTANCE hi = GetModuleHandleW(NULL);
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = wndproc; wc.hInstance = hi; wc.lpszClassName = L"d3d11tri";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, L"d3d11tri", L"d3d11tri", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                40, 40, 800, 600, NULL, NULL, hi, NULL);
    if (!hwnd) { printf("CreateWindow failed\n"); return 1; }

    DXGI_SWAP_CHAIN_DESC sd = {0};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 800; sd.BufferDesc.Height = 600;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60; sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ID3D11Device *dev = NULL; ID3D11DeviceContext *ctx = NULL; IDXGISwapChain *sc = NULL;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
                                               D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
    if (FAILED(hr)) { printf("D3D11CreateDeviceAndSwapChain failed 0x%08lx\n", (unsigned long)hr); return 2; }

    IDXGIDevice *dxgidev = NULL; IDXGIAdapter *adapter = NULL;
    if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&dxgidev)) &&
        SUCCEEDED(IDXGIDevice_GetAdapter(dxgidev, &adapter))) {
        DXGI_ADAPTER_DESC ad;
        if (SUCCEEDED(IDXGIAdapter_GetDesc(adapter, &ad)))
            printf("adapter: %ls vendor=%04x device=%04x vram=%lluMB feature_level=0x%x\n",
                   ad.Description, ad.VendorId, ad.DeviceId,
                   (unsigned long long)(ad.DedicatedVideoMemory >> 20), (unsigned)fl);
        IDXGIAdapter_Release(adapter);
    }
    if (dxgidev) IDXGIDevice_Release(dxgidev);

    ID3DBlob *vsb = NULL, *psb = NULL, *err = NULL;
    if (FAILED(D3DCompile(g_shader, strlen(g_shader), NULL, NULL, NULL, "vs", "vs_4_0", 0, 0, &vsb, &err))) {
        printf("vs compile failed: %s\n", err ? (char *)ID3D10Blob_GetBufferPointer(err) : "?"); return 3;
    }
    if (FAILED(D3DCompile(g_shader, strlen(g_shader), NULL, NULL, NULL, "ps", "ps_4_0", 0, 0, &psb, &err))) {
        printf("ps compile failed: %s\n", err ? (char *)ID3D10Blob_GetBufferPointer(err) : "?"); return 3;
    }
    ID3D11VertexShader *vs = NULL; ID3D11PixelShader *ps = NULL;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &vs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &ps);

    D3D11_BUFFER_DESC bd = {0};
    bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer *cb = NULL;
    ID3D11Device_CreateBuffer(dev, &bd, NULL, &cb);

    ID3D11Texture2D *bb = NULL; ID3D11RenderTargetView *rtv = NULL;
    IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&bb);
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, NULL, &rtv);
    ID3D11Texture2D_Release(bb);

    D3D11_VIEWPORT vp = {0, 0, 800, 600, 0, 1};
    LARGE_INTEGER f0, f1, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&f0);
    int n = 0;
    MSG msg;
    while (n < frames) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        float t = (float)n / 60.0f;
        float clear[4] = {0.08f + 0.05f * (float)(n % 20) / 20.0f, 0.10f, 0.16f, 1.0f};
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
        ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, clear);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            float tint[4] = {0.6f + 0.4f * (float)((n / 30) % 2), 0.6f + 0.4f * (float)((n / 60) % 2), 1.0f, t};
            memcpy(m.pData, tint, sizeof tint);
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)cb, 0);
        }
        ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
        ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &cb);
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_Draw(ctx, 3, 0);
        IDXGISwapChain_Present(sc, 1, 0);
        n++;
        if (n % 60 == 0) { printf("frame %d\n", n); fflush(stdout); }
    }
done:
    QueryPerformanceCounter(&f1);
    double secs = (double)(f1.QuadPart - f0.QuadPart) / (double)freq.QuadPart;
    printf("d3d11tri: %d frames in %.2fs = %.1f fps\n", n, secs, n / (secs > 0 ? secs : 1));
    fflush(stdout);
    return 0;
}
