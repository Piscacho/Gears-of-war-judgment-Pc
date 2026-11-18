#include <unordered_set>
// Backend D3D12 (sesiones 7-8): dispositivo, swapchain, render target "EDRAM", draws traducidos y presentacion.
// Todo se usa desde un unico hilo (el del procesador de comandos).
#include "gpu.h"
#include "gpu_draw.h"
#include "gpu_shader.h"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <psapi.h>
float PanoAspect(); void ApplyPanoCamera();   // panoramico (sesion 25)
uint64_t TexDirtyPageSig(uint32_t addr, uint32_t len);   // texdirty.cpp (GoWJ sesion 11)
#include <atomic>
std::atomic<bool> g_scriptF9{ false };
int g_dbgMemexPending = 0; uint64_t g_dbgSkipSnap[16];   // diagnostico GOWJ_MEMEXLOG (sesion 11)   // GoWJ sesion 11: el guion de entrada pide registrar el cuadro (como F9)
bool TexDirtyPageValid(uint32_t addr, uint32_t len, uint64_t sig);
// Sesion 27: el panoramico solo se aplica en los frames con escena 3D (dibujos en la superficie de la escena, pitch 1120). Los
// videos, menus y pantallas de carga (solo 2D de 16:9) se presentan a 16:9 con bandas negras en vez de estirarse.
// g_pano3DFrames = frames seguidos sin escena (con margen de 30 frames para no parpadear).
static bool g_frameHasScene = false; static int g_framesNoScene = 1000;
static bool g_frameHasMovie = false;   // GoWJ sesion 11: el cuadro dibuja un video Bink (planos k_8 de 1280x704)
static bool PanoActive() { return g_framesNoScene < 30; }
// Sesion 27: panoramico nitido. Las superficies proporcionales a la pantalla (pitch 1120/560/280 de la escena y 1280/640/320
// de la imagen final) se crean S veces mas anchas (S = kx redondeado hacia arriba: 2 en 21:9 y 32:9). Todo lo que va en pixeles
// de la superficie (viewport, recortes, borrados, resolves) se escala por S; las texturas resueltas quedan S veces mas anchas
// (ResolvedTex::sx) y las lecturas en texeles y PsParamGen lo compensan. GOWJ_PANOSTRETCH=1 = antes (1280x720 estirado).
uint32_t PanoWide();
// GoWJ sesion 11: 1120/560/280 son las superficies de escena de NG2; en Judgment la escena va en 1280 y en 1120/560 se dibujan atlas
// de luces/sombras con teselas (96x32, 256x192, subrectangulos en x=95) que no deben ensancharse. GOWJ_PANO1120=1 = antes.
static inline bool ScreenPitch(uint32_t p)
{
    static const bool old1120 = getenv("GOWJ_PANO1120") != nullptr;
    static const int mask = getenv("GOWJ_PANOPITCHMASK") ? atoi(getenv("GOWJ_PANOPITCHMASK")) : 7;   // diagnostico: 1 = 1280, 2 = 640, 4 = 320
    return (old1120 && (p == 1120 || p == 560 || p == 280)) || (p == 1280 && (mask & 1)) || (p == 640 && (mask & 2)) || (p == 320 && (mask & 4));
}
// GoWJ sesion 11: resolucion interna (GOWJ_RESINTERNA=1..3, resolucion= en gowj.ini): las mismas superficies de pantalla se crean
// k veces mas grandes tambien en vertical (sy) y k veces mas en horizontal (sx = ancho del panoramico x k). 1 = como antes.
uint32_t ResK(); uint32_t ScreenScaleX();
static inline uint32_t SurfS(uint32_t p) { return ScreenPitch(p) ? PanoWide() * ResK() : 1u; }
static inline uint32_t SurfSy(uint32_t p) { return ScreenPitch(p) ? ResK() : 1u; }
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "dxgi.lib")
#include <wrl/client.h>
#include <cstring>
#include <cstdarg>
#include <vector>
#include <unordered_map>
#include <map>
#include <string>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <functional>

using Microsoft::WRL::ComPtr;
void CmdTrace(const char* fmt, ...);
float DecodeDepth24(uint32_t v24, bool isFloat);
// float -> half (redondeo simple, sin desnormales)
static uint16_t FloatToHalf(float f)
{
    uint32_t x; memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000; int e = (int)((x >> 23) & 0xFF) - 127 + 15; uint32_t m = x & 0x7FFFFF;
    if (((x >> 23) & 0xFF) == 0xFF) return (uint16_t)(sign | 0x7C00 | (m ? 0x200 : 0));
    if (e <= 0) return (uint16_t)sign;
    if (e >= 31) return (uint16_t)(sign | 0x7C00);
    return (uint16_t)(sign | (((uint32_t)e << 10) + ((m + 0x1000) >> 13)));   // el acarreo del redondeo sube el exponente
}

extern bool g_xlatNoGrad;   // gpu_shader.cpp (sesion 24)
bool TexDirtyEnabled(); bool TexDirtyCheck(uint32_t phys, uint32_t len); void TexDirtyArm(uint32_t phys, uint32_t len);
uint64_t TexDirtyArmSig(uint32_t phys, uint32_t len); bool TexDirtyValid(uint32_t phys, uint32_t len, uint64_t sig);   // texdirty.cpp (sesion 10)
namespace
{
constexpr UINT kFrames = 2;
constexpr UINT kRtW = 1280, kRtH = 720;
constexpr UINT64 kRingSize = 128ull << 20; // por frame (sesion 8 de NG2: 64 MB se llenaban en la escena 3D)
constexpr UINT kDescPerDraw = 48;          // 16 SRV 2D + 16 SRV 3D + 16 SRV cubo
constexpr UINT kDescPerFrame = kDescPerDraw * 8192;  // 8192 draws por frame
ComPtr<ID3D12Device> g_dev;
ComPtr<ID3D12CommandQueue> g_queue;
ComPtr<IDXGISwapChain3> g_swap;
ComPtr<ID3D12DescriptorHeap> g_rtvHeap, g_srvHeap;
ComPtr<ID3D12Resource> g_backbuffers[kFrames];
ComPtr<ID3D12CommandAllocator> g_alloc[kFrames];
ComPtr<ID3D12GraphicsCommandList> g_list;
ComPtr<ID3D12Fence> g_fence;
UINT64 g_fenceValues[kFrames] = {}, g_nextFence = 1;
HANDLE g_fenceEvent = nullptr;
UINT g_rtvSize = 0, g_srvSize = 0;
ComPtr<ID3D12RootSignature> g_blitRootSig, g_drawRootSig;
extern ComPtr<ID3D12PipelineState> g_overlayPso;   // contador en pantalla (sesion 25)
ComPtr<ID3D12PipelineState> g_blitPso, g_blitPsoScene;   // GoWJ sesion 11: Scene = brillo de las imagenes de partida
ComPtr<ID3D12Resource> g_fbTexture, g_fbUpload[kFrames];
UINT g_fbW = 0, g_fbH = 0;
ComPtr<ID3D12Resource> g_edram, g_white, g_ring[kFrames];
uint8_t* g_ringMap[kFrames] = {};
UINT64 g_ringOffset = 0;
struct VbCacheEntry { UINT64 off; uint64_t hash; uint64_t full = 0; };
std::unordered_map<uint64_t, VbCacheEntry> g_vbCache;   // (direccion, bytes) -> offset en el anillo del frame actual + huella del contenido
std::vector<ComPtr<ID3D12Resource>> g_deferredRelease[kFrames];   // recursos a liberar cuando la GPU termine ese frame
bool g_failed = false, g_frameOpen = false;
UINT g_fi = 0;
uint64_t g_frameDraws = 0;
uint64_t g_curPsHash = 0;   // PS del draw en curso (diagnostico)
UINT g_descCursor = 0;
uint64_t g_frameCounter = 0;
ComPtr<ID3DBlob> g_rectGs, g_uiGs, g_rectGsPano;   // sesion 25: GS del panoramico
// ---- EDRAM (sesion 9): un RT por (base, pitch) ----
// Sesion 21: cada superficie tiene el formato de RB_COLOR_INFO (bits 16-19), no siempre BGRA8
struct EdramRt
{
    ComPtr<ID3D12Resource> res; UINT w = 0, h = 0, rtv = 0, base = 0, sx = 1, sy = 1;   // sx: ancho extra del panoramico (sesion 27); sy: alto extra (resolucion interna)
    uint32_t xfmt = 0; DXGI_FORMAT fmt = DXGI_FORMAT_B8G8R8A8_UNORM; UINT bpp = 4; float outScale = 0;
};
// Formato de color de Xenos -> DXGI. Las superficies 16_16 / 16_16_16_16 de Xenos son de punto fijo -32..32: SNORM con la salida escalada 1/32.
void EdramFormat(uint32_t xfmt, DXGI_FORMAT& f, UINT& bpp, float& scale)
{
    scale = 0; bpp = 4;
    switch (xfmt)
    {
    case 2: f = DXGI_FORMAT_R10G10B10A2_UNORM; break;                               // 2_10_10_10
    case 3: case 12: f = DXGI_FORMAT_R16G16B16A16_FLOAT; bpp = 8; break;            // 2_10_10_10_FLOAT (aprox.)
    case 4: f = DXGI_FORMAT_R16G16_SNORM; scale = 1.0f / 32; break;                 // 16_16
    case 5: f = DXGI_FORMAT_R16G16B16A16_SNORM; bpp = 8; scale = 1.0f / 32; break;  // 16_16_16_16
    case 6: f = DXGI_FORMAT_R16G16_FLOAT; break;                                    // 16_16_FLOAT
    case 7: f = DXGI_FORMAT_R16G16B16A16_FLOAT; bpp = 8; break;                     // 16_16_16_16_FLOAT
    case 14: f = DXGI_FORMAT_R32_FLOAT; break;                                      // 32_FLOAT
    case 15: f = DXGI_FORMAT_R32G32_FLOAT; bpp = 8; break;                          // 32_32_FLOAT
    default: f = DXGI_FORMAT_B8G8R8A8_UNORM; break;                                 // 8888 y 8888_GAMMA
    }
}
std::unordered_map<uint32_t, EdramRt> g_edramRts;
static const bool g_noDepthEnv = getenv("GOWJ_NODEPTH") != nullptr;
EdramRt* g_boundRt = nullptr;
ComPtr<ID3D12Resource> g_readback;
uint64_t g_resolves = 0, g_fbResolves = 0;
constexpr UINT kEdramH = 1024;
// ---- profundidad (sesion 19): una superficie D32+S8 por (base de depth, pitch) ----
struct DepthRt { ComPtr<ID3D12Resource> res; UINT w = 0, h = 0, dsv = 0, sx = 1, sy = 1; };
std::unordered_map<uint32_t, DepthRt> g_depthRts;
ComPtr<ID3D12DescriptorHeap> g_dsvHeap;
UINT g_dsvSize = 0;
// Sesion 24: profundidad D24 (como la EDRAM de la 360) en vez de D32 float. El juego restaura la profundidad del frame anterior
// (palabra D24S8) y vuelve a dibujar personajes y luces con LEQUAL: con D32 float la rejilla de redondeo no coincidia con la de
// 24 bits y la prueba fallaba en columnas periodicas de 1 px (rayado fino en los personajes). GOWJ_DEPTH32=1 = antes.
static const bool g_depth32 = getenv("GOWJ_DEPTH32") != nullptr;
static const DXGI_FORMAT kDepthFmt = g_depth32 ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D24_UNORM_S8_UINT;
static const DXGI_FORMAT kDepthSrvFmt = g_depth32 ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R24_UNORM_X8_TYPELESS;

struct ShaderEntry { bool ok = false; ComPtr<ID3DBlob> blob; TranslatedShader info; std::atomic<int> state{ 0 }; };   // state: 0 sin compilar, 1 compilando (asincrono), 2 listo
std::unordered_map<uint64_t, ShaderEntry> g_shaders;
std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> g_psos;
// Sesion 23 (rendimiento): biblioteca de PSO en disco (shadercache/pso.lib, ID3D12PipelineLibrary). Las partidas siguientes cargan
// los PSO ya vistos en vez de crearlos (61 PSO nuevos en un corte de escena = 22 ms). Si el controlador no acepta el archivo
// (otra version), se empieza una biblioteca vacia. Medido: sin mejora (el controlador ya cachea); solo con GOWJ_PSOLIB=1.
static ComPtr<ID3D12PipelineLibrary> g_psoLib; static std::vector<uint8_t> g_psoLibBlob; static int g_psoLibDirty = 0;
static void SavePsoLib()
{
    if (!g_psoLib || !g_psoLibDirty) return;
    SIZE_T n = g_psoLib->GetSerializedSize(); std::vector<uint8_t> buf(n);
    if (n && SUCCEEDED(g_psoLib->Serialize(buf.data(), n)))
    {
        CreateDirectoryA("shadercache", nullptr);
        if (FILE* f = fopen("shadercache/pso.tmp", "wb")) { fwrite(buf.data(), 1, n, f); fclose(f); MoveFileExA("shadercache/pso.tmp", "shadercache/pso.lib", MOVEFILE_REPLACE_EXISTING); }
    }
    g_psoLibDirty = 0;
}
static HRESULT CreatePsoCached(uint64_t key, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& pd, ComPtr<ID3D12PipelineState>& out)
{
    static const bool off = getenv("GOWJ_PSOLIB") == nullptr;   // por defecto desactivada: el controlador ya cachea (sin mejora medida)
    static bool init = false;
    if (!off && !init)
    {
        init = true;
        ComPtr<ID3D12Device1> d1;
        if (SUCCEEDED(g_dev.As(&d1)))
        {
            if (FILE* f = fopen("shadercache/pso.lib", "rb")) { fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); if (n > 0) { g_psoLibBlob.resize(n); if (fread(g_psoLibBlob.data(), 1, n, f) != (size_t)n) g_psoLibBlob.clear(); } fclose(f); }
            HRESULT hr = g_psoLibBlob.empty() ? E_FAIL : d1->CreatePipelineLibrary(g_psoLibBlob.data(), g_psoLibBlob.size(), IID_PPV_ARGS(&g_psoLib));
            if (FAILED(hr)) { g_psoLibBlob.clear(); d1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&g_psoLib)); }
            LOG("[d3d12] biblioteca de PSO: %s (%zu bytes)", g_psoLib ? (SUCCEEDED(hr) ? "cargada" : "nueva") : "no disponible", g_psoLibBlob.size());
        }
    }
    if (!g_psoLib) return g_dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&out));
    wchar_t name[24]; swprintf(name, 24, L"%016llX", (unsigned long long)key);
    if (SUCCEEDED(g_psoLib->LoadGraphicsPipeline(name, &pd, IID_PPV_ARGS(&out)))) return S_OK;
    HRESULT hr = g_dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&out));
    if (SUCCEEDED(hr) && SUCCEEDED(g_psoLib->StorePipeline(name, out.Get())) && ++g_psoLibDirty >= 24) SavePsoLib();
    return hr;
}

// Copia de profundidad: constantes = origen en muestras (x, y) y paso de muestreo (msx, msy)
const char* kDepthCopyHlsl = R"(
cbuffer C : register(b0) { int4 off; };
Texture2D<float> D : register(t0);
struct O { float z : SV_Target0; float4 p : SV_Target1; };
float4 vs_main(uint id : SV_VertexID) : SV_Position { float2 uv = float2((id << 1) & 2, id & 2); return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }
O ps_main(float4 pos : SV_Position)
{
    int2 xy = int2(pos.xy) * off.zw + off.xy;
    float z = D.Load(int3(xy, 0));
    uint v = (uint)(saturate(z) * 16777215.0 + 0.5);
    O o; o.z = z; o.p = float4(0, v & 255, (v >> 8) & 255, (v >> 16) & 255) / 255.0;
    return o;
}
)";
ComPtr<ID3D12RootSignature> g_depthCopyRootSig;
ComPtr<ID3D12PipelineState> g_depthCopyPso;
ComPtr<ID3D12DescriptorHeap> g_rtvHeap2;
// Sesion 24 (rendimiento): resolve de color con MSAA por GPU. off.xy = origen en muestras, off.zw = muestras por pixel (msx, msy),
// mode.x = muestra/media (RB_COPY_CONTROL bits 4-6: 4 = media de 0/1, 5 = media de 2/3, 6 = media de las 4; <4 = muestra 0,
// como la ruta de CPU). Muestras en la superficie: (0,0) (1,0) (0,1) (1,1) dentro de cada bloque msx x msy.
const char* kColorResolveHlsl = R"(
cbuffer C : register(b0) { int4 off; int4 mode; };
Texture2D<float4> S : register(t0);
float4 vs_main(uint id : SV_VertexID) : SV_Position { float2 uv = float2((id << 1) & 2, id & 2); return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }
// GoWJ sesion 8: float -> bits float7e3 de Xenos (como Float32To7e3 de la CPU)
float F7e3(float f)
{
    if (!(f > 0)) return 0;
    if (f >= 31.875) return 1023;
    uint u = asuint(f);
    if (u < 0x3E800000) { uint sh = 125 - (u >> 23); u = sh >= 24 ? 0 : (0x800000 | (u & 0x7FFFFF)) >> sh; }
    else u += 0xC2000000;
    return (float)(((u + 0x7FFF + ((u >> 16) & 1)) >> 16) & 0x3FF);
}
float4 ps_main(float4 pos : SV_Position) : SV_Target0
{
    int2 b = int2(pos.xy) * off.zw + off.xy;
    float4 s0 = S.Load(int3(b, 0));
    // GoWJ sesion 8: escena HDR (RGBA16F) -> 2_10_10_10 con los bits 7e3 (resolve "en bruto" que el tonemap lee como fmt 54).
    // mode.z = escala (asfloat, sesgo de exponente), mode.w = intercambio R/B. Destino R10G10B10A2_UNORM (bits / 1023).
    if (mode.y == 3)
    {
        float4 v = s0 * asfloat(mode.z); if (mode.w) v.rgb = v.bgr;
        float a = isnan(v.a) ? 0 : round(saturate(v.a) * 3.0) / 3.0;
        return float4(F7e3(v.r) / 1023.0, F7e3(v.g) / 1023.0, F7e3(v.b) / 1023.0, a);
    }
    // GoWJ sesion 8: conversion generica (posproceso 16_16_16_16 -> 26/32/31): valor * mode.z (asfloat); mode.w bit 0 = intercambio R/B
    if (mode.y == 4) { float4 v = s0 * asfloat(mode.z); if (mode.w & 1) v.rgb = v.bgr; return v; }
    if (mode.y == 1) return float4(saturate(s0.xy * 32.0), 0, 0);   // 16_16 (-32..32 en SNORM/32) -> k_16_16 sin signo
    // Sesion 24: sin recortar (textura R16G16_FLOAT): el SSAO guarda en .y la distancia/64, que pasa de 1 en lo lejano; al
    // recortarla a 1 el suavizado bilateral mezclaba fondo y primer plano (bloques y halos en escalera)
    if (mode.y == 2) return float4(s0.xy * 32.0, 0, 0);
    int sel = mode.x;
    if (sel < 4 || sel > 6) return s0;
    int2 dx = int2(off.z > 1 ? 1 : 0, 0), dy = int2(0, off.w > 1 ? 1 : 0);
    float4 s1 = S.Load(int3(b + dx, 0)), s2 = S.Load(int3(b + dy, 0)), s3 = S.Load(int3(b + dx + dy, 0));
    if (sel == 4) { if (off.w > 1 && off.z == 1) s1 = s2; return (s0 + s1) * 0.5; }
    if (sel == 5) return (s2 + s3) * 0.5;
    return (s0 + s1 + s2 + s3) * 0.25;
}
)";
// Sesion 24: aliasing de EDRAM color -> profundidad. En la 360 color y profundidad comparten la EDRAM: el juego restaura la
// profundidad del frame anterior escribiendo COLOR (8888 con la palabra D24S8: G = bits 0-7, B = 8-15, A = 16-23 de la
// profundidad, R = stencil) en la base del buffer de profundidad (PS C316F389, RT0 en la base de RB_DEPTH_INFO). Aqui color
// y profundidad son recursos distintos: tras ese draw se convierte el color escrito en profundidad con este pase.
const char* kColorToDepthHlsl = R"(
Texture2D<float4> S : register(t0);
float4 vs_main(uint id : SV_VertexID) : SV_Position { float2 uv = float2((id << 1) & 2, id & 2); return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }
float ps_main(float4 pos : SV_Position) : SV_Depth
{
    int2 p = int2(pos.xy);
#ifndef NO_SWAP40
    // En la EDRAM, las baldosas de profundidad tienen las dos columnas de 40 muestras intercambiadas respecto a las de color
    // (como Xenia al copiar entre color y profundidad). Sin esto cada franja de 40 px recibia la profundidad de su vecina.
    uint w, h; S.GetDimensions(w, h);
#ifndef SWAPK
#define SWAPK 1
#endif
    int tx = (p.x / SWAPK) % 80; int sx = p.x + (tx < 40 ? 40 : -40) * SWAPK;
    if (sx >= 0 && sx < (int)w) p.x = sx;
#endif
    float4 c = S.Load(int3(p, 0));
    uint v = ((uint)round(c.a * 255.0) << 16) | ((uint)round(c.b * 255.0) << 8) | (uint)round(c.g * 255.0);
    return v / 16777215.0;
}
)";
ComPtr<ID3D12RootSignature> g_colorToDepthRootSig;
ComPtr<ID3D12PipelineState> g_colorToDepthPso, g_colorToDepthPsoK;   // K: superficie panoramica (sesion 27)
ComPtr<ID3D12RootSignature> g_colorResolveRootSig;
ComPtr<ID3D12PipelineState> g_colorResolvePso, g_colorResolvePso1616, g_colorResolvePso1616F, g_colorResolvePso16F, g_colorResolvePso565, g_colorResolvePso7e3, g_colorResolvePsoU16;
static const bool g_1616Sat = getenv("GOWJ_1616SAT") != nullptr;   // diagnostico: 16_16 -> k_16_16 recortado a 0..1 (antes)
ComPtr<ID3D12DescriptorHeap> g_rtvHeap3;   // RTV de las texturas resueltas por shader
constexpr UINT kRtvHeap3Size = 512;
UINT g_rtvHeap3Next = 0;
const char* kBlitHlsl = R"(
Texture2D t0 : register(t0);
SamplerState s0 : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs_main(uint id : SV_VertexID)
{
    V o; float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv; o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
#ifndef PORT_GAMMA
#define PORT_GAMMA 1.0
#endif
// GoWJ sesion 11: brillo del port (gamma final, como en el port de Gears 3; 1,0 = como la consola)
float3 PortGamma(float3 c) { return PORT_GAMMA == 1.0 ? c : pow(saturate(c), 1.0 / PORT_GAMMA); }
float4 ps_main(V i) : SV_Target { return float4(PortGamma(t0.Sample(s0, i.uv).rgb), 1); }
float4 ps_overlay(V i) : SV_Target { return t0.SampleLevel(s0, i.uv, 0); }   // sesion 25: contador (alfa premultiplicado)
// Sesion 24: escalado nitido a la ventana (Catmull-Rom con 9 lecturas bilineales). A 1:1 reproduce la imagen exacta.
float4 ps_cubic(V i) : SV_Target
{
    float2 ts; t0.GetDimensions(ts.x, ts.y);
    float2 sp = i.uv * ts, tp1 = floor(sp - 0.5) + 0.5, f = sp - tp1;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2, tp12 = (tp1 + w2 / w12) / ts, tp0 = (tp1 - 1.0) / ts, tp3 = (tp1 + 2.0) / ts;
    float3 c = 0;
    c += t0.SampleLevel(s0, float2(tp0.x, tp0.y), 0).rgb * w0.x * w0.y;
    c += t0.SampleLevel(s0, float2(tp12.x, tp0.y), 0).rgb * w12.x * w0.y;
    c += t0.SampleLevel(s0, float2(tp3.x, tp0.y), 0).rgb * w3.x * w0.y;
    c += t0.SampleLevel(s0, float2(tp0.x, tp12.y), 0).rgb * w0.x * w12.y;
    c += t0.SampleLevel(s0, float2(tp12.x, tp12.y), 0).rgb * w12.x * w12.y;
    c += t0.SampleLevel(s0, float2(tp3.x, tp12.y), 0).rgb * w3.x * w12.y;
    c += t0.SampleLevel(s0, float2(tp0.x, tp3.y), 0).rgb * w0.x * w3.y;
    c += t0.SampleLevel(s0, float2(tp12.x, tp3.y), 0).rgb * w12.x * w3.y;
    c += t0.SampleLevel(s0, float2(tp3.x, tp3.y), 0).rgb * w3.x * w3.y;
#ifdef SHARPEN
    // GoWJ sesion 11: nitidez adaptativa al contraste (como CAS de AMD) con los 4 vecinos a un texel de la imagen del juego: compensa
    // el desenfoque del FXAA del juego y del escalado (1280x720 -> 5120x1440). Menos efecto donde ya hay mucho contraste (sin halos).
    {
        float2 px = 1.0 / ts;
        float3 a = t0.SampleLevel(s0, i.uv + float2(0, -px.y), 0).rgb, b = t0.SampleLevel(s0, i.uv + float2(-px.x, 0), 0).rgb;
        float3 d = t0.SampleLevel(s0, i.uv + float2(px.x, 0), 0).rgb, e = t0.SampleLevel(s0, i.uv + float2(0, px.y), 0).rgb;
        float3 m = saturate(c);
        float3 mn = min(m, min(min(a, b), min(d, e))), mx = max(m, max(max(a, b), max(d, e)));
        float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-5)));
        float3 w = amp * (-1.0 / lerp(8.0, 5.0, SHARP));
        c = (m + (a + b + d + e) * w) / (1.0 + 4.0 * w);
    }
#endif
    return float4(PortGamma(saturate(c)), 1);
}
)";
// GoWJ sesion 11: laterales de las pantallas que se presentan a 16:9 en un monitor mas ancho (videos, cargas, resultados, menus).
// En vez de barras negras, el borde de la imagen continua hacia fuera en espejo, cada vez mas desenfocado y oscuro (como el "modo
// ambiente" de los reproductores): el salto entre la parte 16:9 y el resto del monitor deja de ser un corte duro.
// R.x / R.y = columnas (px) del borde izquierdo / derecho de la imagen, R.z = ancho de cada lateral (px), R.w = alto (px).
const char* kSideHlsl = R"(
Texture2D t0 : register(t0);
SamplerState s0 : register(s0);
cbuffer C : register(b0) { float4 R; };
float4 vs_main(uint id : SV_VertexID) : SV_Position { float2 uv = float2((id << 1) & 2, id & 2); return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }
float4 ps_main(float4 pos : SV_Position) : SV_Target
{
    bool left = pos.x < R.x;
    float d = saturate(left ? (R.x - pos.x) / R.z : (pos.x - R.y) / R.z);   // 0 junto a la imagen .. 1 en el borde del monitor
    float y = pos.y / R.w;
    float span = 0.30;                                   // parte de la imagen que se refleja
    float u = left ? d * span : 1.0 - d * span;
    float rad = lerp(0.02, 0.10, sqrt(d));              // desenfoque creciente (en unidades de la imagen)
    float3 c = 0; float wsum = 0;
    [unroll] for (int j = -3; j <= 3; j++)
    [unroll] for (int k = -3; k <= 3; k++)
    {
        float w = exp(-(j * j + k * k) / 8.0);
        c += t0.SampleLevel(s0, saturate(float2(u + k * rad * 0.28, y + j * rad * 0.5)), 0).rgb * w; wsum += w;
    }
    c /= wsum;
    float b = lerp(0.85, 0.16, pow(d, 0.8));
    return float4(c * b, 1);
}
)";
ComPtr<ID3D12RootSignature> g_sideRootSig; ComPtr<ID3D12PipelineState> g_sidePso;
// Tamano actual de la cadena de intercambio (sigue al de la ventana) y ventana de presentacion (sesion 24)
UINT g_bbW = kRtW, g_bbH = kRtH;
HWND g_presentHwnd = nullptr;
UINT g_swapFlags = 0;
HANDLE g_frameWait = nullptr;   // objeto de espera de latencia de la cadena de intercambio

// Rect list: 3 esquinas -> quad. La diagonal es la arista mas larga; el 4o vertice = a + b - esquina.
const char* kRectGsHlsl = R"(
[maxvertexcount(4)]
void main(triangle XVSOut v[3], inout TriangleStream<XVSOut> s)
{
    float2 p0 = v[0].pos.xy / v[0].pos.w, p1 = v[1].pos.xy / v[1].pos.w, p2 = v[2].pos.xy / v[2].pos.w;
    float d01 = dot(p0 - p1, p0 - p1), d12 = dot(p1 - p2, p1 - p2), d02 = dot(p0 - p2, p0 - p2);
    uint c = 0, a = 1, b = 2;
    if (d02 > d12 && d02 >= d01) { c = 1; a = 0; b = 2; }
    else if (d01 > d12 && d01 > d02) { c = 2; a = 0; b = 1; }
    XVSOut d;
    d.pos = v[a].pos + v[b].pos - v[c].pos;
    [unroll] for (int i = 0; i < 16; i++) d.t[i] = v[a].t[i] + v[b].t[i] - v[c].t[i];
    s.Append(v[c]); s.Append(v[a]); s.Append(v[b]); s.Append(d);
}
)";


// Sesion 25 (panoramico): la interfaz 2D se dibuja en la superficie final de 1280 x 720 y luego se estira a 21:9 / 32:9. Para que no
// se deforme, cada triangulo en coordenadas de pantalla (w = 1) que no va de borde a borde se comprime hacia el centro (x / KX);
// los de borde a borde (la escena compuesta, fondos, fundidos) se quedan igual. KX = proporcion / (16/9), se define al compilar.
const char* kPanoUiFnHlsl = R"(
bool XPanoUi(float4 a, float4 b, float4 c)
{
#ifdef PANO_WSTRICT
    bool flat = abs(a.w - 1.0) < 1e-4 && abs(b.w - 1.0) < 1e-4 && abs(c.w - 1.0) < 1e-4;
#else
    // GoWJ sesion 11: el HUD de Scaleform (estrellas, municion, objetivos) guarda sus vertices con w = 0,1 (coordenadas x 0,1): con
    // "w = 1" no se comprimia y se veia estirado al doble de ancho. Plano = los tres w iguales y positivos (sin perspectiva).
    float wm = max(a.w, max(b.w, c.w));
    bool flat = min(a.w, min(b.w, c.w)) > 0 && (wm - min(a.w, min(b.w, c.w))) < wm * 1e-4;
    a.xy /= a.w; b.xy /= b.w; c.xy /= c.w;
#endif
    float mn = min(a.x, min(b.x, c.x)), mx = max(a.x, max(b.x, c.x));
#ifndef PANO_NO_OVERSCAN
    // GoWJ sesion 11: un triangulo que se sale de la pantalla (efectos a pantalla completa hechos de varios trozos, como la tinta de
    // dano, de -1,14 a 1,14) no es interfaz: comprimido quedaba solo en el centro (cortes en x = 550 y 2010 a 32:9).
    float ymn = min(a.y, min(b.y, c.y)), ymx = max(a.y, max(b.y, c.y));
    // Solo para w = 1 (dibujos de UE3): los paneles de Scaleform (w = 0,1) que entran desde fuera de la pantalla (fondo de "DESPEDAZAR")
    // son interfaz y se comprimen con su texto.
    if (abs(a.w - 1.0) < 1e-4 && (mn < -1.02 || mx > 1.02 || ymn < -1.02 || ymx > 1.02)) return false;
#endif
    return flat && (mx - mn) < 1.95;
}
)";
const char* kUiGsHlsl = R"(
[maxvertexcount(3)]
void main(triangle XVSOut v[3], inout TriangleStream<XVSOut> s)
{
    bool ui = XPanoUi(v[0].pos, v[1].pos, v[2].pos);
    [unroll] for (int k = 0; k < 3; k++) { XVSOut o = v[k]; if (ui) o.pos.x /= KX; s.Append(o); }
}
)";
const char* kRectGsPanoHlsl = R"(
[maxvertexcount(4)]
void main(triangle XVSOut v[3], inout TriangleStream<XVSOut> s)
{
    float2 p0 = v[0].pos.xy / v[0].pos.w, p1 = v[1].pos.xy / v[1].pos.w, p2 = v[2].pos.xy / v[2].pos.w;
    float d01 = dot(p0 - p1, p0 - p1), d12 = dot(p1 - p2, p1 - p2), d02 = dot(p0 - p2, p0 - p2);
    uint c = 0, a = 1, b = 2;
    if (d02 > d12 && d02 >= d01) { c = 1; a = 0; b = 2; }
    else if (d01 > d12 && d01 > d02) { c = 2; a = 0; b = 1; }
    XVSOut d;
    d.pos = v[a].pos + v[b].pos - v[c].pos;
    [unroll] for (int i = 0; i < 16; i++) d.t[i] = v[a].t[i] + v[b].t[i] - v[c].t[i];
    bool ui = XPanoUi(v[0].pos, v[1].pos, v[2].pos);
    XVSOut q[4] = { v[c], v[a], v[b], d };
    [unroll] for (int k = 0; k < 4; k++) { if (ui) q[k].pos.x /= KX; s.Append(q[k]); }
}
)";

bool Check(HRESULT hr, const char* what)
{
    if (SUCCEEDED(hr)) return true;
    LOG("[d3d12] fallo en %s (0x%08lX); se vuelve a GDI", what, (unsigned long)hr);
    g_failed = true;
    return false;
}

// Sesion 23: grupo fijo de hilos para trabajo de CPU repartible (destileado de texturas). ParallelFor(n, f) ejecuta f(0..n-1):
// f(0) en el hilo que llama y el resto en los trabajadores; vuelve cuando terminan todos. Una sola llamada a la vez.
static void ParallelFor(unsigned n, const std::function<void(unsigned)>& f)
{
    struct Pool
    {
        std::mutex m; std::condition_variable cv, done; const std::function<void(unsigned)>* job = nullptr;
        unsigned count = 0, pending = 0; uint64_t gen = 0; std::vector<std::thread> th;
        explicit Pool(unsigned w)
        {
            for (unsigned i = 0; i < w; i++)
                th.emplace_back([this, i] {
                    uint64_t seen = 0;
                    for (;;)
                    {
                        std::unique_lock<std::mutex> l(m);
                        cv.wait(l, [&] { return gen != seen; });
                        seen = gen;
                        if (i + 1 >= count) continue;   // este trabajador no participa en esta ronda
                        auto* j = job; l.unlock();
                        (*j)(i + 1);
                        l.lock(); if (--pending == 0) done.notify_one();
                    }
                });
        }
    };
    static Pool pool(7);
    static std::mutex callLock;
    if (n <= 1) { f(0); return; }
    n = std::min(n, 8u);
    std::lock_guard<std::mutex> cl(callLock);
    { std::lock_guard<std::mutex> l(pool.m); pool.job = &f; pool.count = n; pool.pending = n - 1; pool.gen++; }
    pool.cv.notify_all();
    f(0);
    std::unique_lock<std::mutex> l(pool.m);
    pool.done.wait(l, [&] { return pool.pending == 0; });
}
// Sesion 24: medicion de tirones (GOWJ_HITCHMS=<ms>, por defecto 40) y cache de shaders compilados en disco
double QpcMs()
{
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c); return double(c.QuadPart) * 1000.0 / double(f.QuadPart);
}
uint64_t g_psoAsyncSkips = 0; double g_hCompileMs = 0, g_hPsoMs = 0, g_hTexMs = 0, g_hWaitMs = 0, g_hTexCreateMs = 0, g_hTexFillMs = 0; uint32_t g_hTexNew = 0, g_hTexUp = 0;
uint32_t g_hCompiles = 0, g_hCacheHits = 0, g_hPsos = 0, g_hFlushes = 0;
double g_hFlushMs = 0, g_hDrawMs = 0, g_hPresentMs = 0;
thread_local bool t_prewarm = false;   // hilos de precompilacion: no cuentan en la medicion de tirones
thread_local bool t_cacheOnly = false; // sesion 23: Compile() solo mira la cache en disco (sin compilar)
std::mutex g_xlatMutex;                // el traductor usa estado global (g_curIsPs, g_xlatNoGrad)
uint64_t Fnv64(const void* d, size_t n, uint64_t h = 1469598103934665603ull)
{
    const uint8_t* b = (const uint8_t*)d;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
// Llave de las caches de texturas (GH sesion 24): con XOR dos texturas distintas compartian recurso -> Close E_INVALIDARG -> DEVICE_HUNG
static uint64_t TexKey(uint32_t base, uint32_t fmt, uint32_t w, uint32_t h, uint32_t extra, uint32_t kind)
{ uint32_t v[6] = { base, fmt, w, h, extra, kind }; return Fnv64(v, sizeof(v)); }

// Sesion 24: vertex shaders con IEEE estricto (sin fusionar ni reordenar operaciones). El juego vuelve a dibujar la misma geometria
// con otro VS (p. ej. la luz de los focos 031FA1CF sobre el suelo, con prueba de profundidad LEQUAL); con las mismas operaciones de
// Xenos, la posicion debe salir identica al bit. Optimizado, el driver fusionaba de forma distinta y la prueba fallaba a franjas.
// No arreglo nada visible en las pruebas: queda como opcion (GOWJ_STRICTVS=1) para no recompilar todos los VS.
static bool StrictVs(const char* target) { static const bool on = getenv("GOWJ_STRICTVS") != nullptr; return on && target[0] == 'v' && target[1] == 's'; }
ComPtr<ID3DBlob> Compile(const std::string& src, const char* entry, const char* target, const char* name)
{
    struct CompileTimer { double t0 = QpcMs(); ~CompileTimer() { if (!t_prewarm) g_hCompileMs += QpcMs() - t0; } } ctimer;
    ComPtr<ID3DBlob> code, err;
    if (getenv("GOWJ_HLSLDUMP"))   // GOWJ_HLSLDUMP=1: guarda cada shader traducido en hlsl_dump/ (sesion 24)
    {
        CreateDirectoryA("hlsl_dump", nullptr);
        char path[128]; snprintf(path, sizeof(path), "hlsl_dump/%s.hlsl", name);
        if (FILE* f = fopen(path, "wb")) { fwrite(src.data(), 1, src.size(), f); fclose(f); }
    }
    // Cache en disco (shadercache\<hash del HLSL + destino>.cso): las siguientes partidas no recompilan. GOWJ_NOSHADERCACHE=1 la desactiva.
    static const bool noCache = getenv("GOWJ_NOSHADERCACHE") != nullptr;
    char cpath[96] = {};
    if (!noCache)
    {
        uint64_t key = Fnv64(src.data(), src.size());
        key = Fnv64(target, strlen(target), key); key = Fnv64(entry, strlen(entry), key);
        key = StrictVs(target) ? Fnv64("O1-v1-ieee", 10, key) : Fnv64("O1-v1", 5, key);
        snprintf(cpath, sizeof(cpath), "shadercache/%016llX.cso", (unsigned long long)key);
        HANDLE h = CreateFileA(cpath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            LARGE_INTEGER sz{}; GetFileSizeEx(h, &sz);
            DWORD got = 0;
            if (sz.QuadPart > 0 && sz.QuadPart < (64 << 20) && SUCCEEDED(D3DCreateBlob((SIZE_T)sz.QuadPart, &code)) &&
                ReadFile(h, code->GetBufferPointer(), (DWORD)sz.QuadPart, &got, nullptr) && got == sz.QuadPart)
            { CloseHandle(h); if (!t_prewarm) g_hCacheHits++; return code; }
            CloseHandle(h); code.Reset();
        }
    }
    if (t_cacheOnly) return nullptr;
    if (!t_prewarm) g_hCompiles++;
    HRESULT hrc = D3DCompile(src.data(), src.size(), name, nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL1 | (StrictVs(target) ? D3DCOMPILE_IEEE_STRICTNESS : 0), 0, &code, &err);
    if (FAILED(hrc))
    {
        // fxc tiene errores internos ("invalid access of unbound variable") con XDot* dentro del bucle de control de flujo:
        // segundo intento con dot() nativo (pierde solo la regla 0 * inf = 0 en los productos escalares)
        std::string alt = src;
        auto repl = [&](const char* a, const char* b) { size_t q = alt.find(a); if (q != std::string::npos) alt.replace(q, strlen(a), b); };
        repl("float XDot4(float4 a, float4 b) { float4 m = XMul(a, b); return m.x + m.y + m.z + m.w; }", "float XDot4(float4 a, float4 b) { return dot(a, b); }");
        repl("float XDot3(float4 a, float4 b) { float4 m = XMul(a, b); return m.x + m.y + m.z; }", "float XDot3(float4 a, float4 b) { return dot(a.xyz, b.xyz); }");
        repl("float XDot2(float4 a, float4 b) { float4 m = XMul(a, b); return m.x + m.y; }", "float XDot2(float4 a, float4 b) { return dot(a.xy, b.xy); }");
        ComPtr<ID3DBlob> err2;
        if (alt != src) hrc = D3DCompile(alt.data(), alt.size(), name, nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL1 | (StrictVs(target) ? D3DCOMPILE_IEEE_STRICTNESS : 0), 0, &code, &err2);
        if (SUCCEEDED(hrc)) LOG("[d3d12] %s compilado con dot() nativo (error interno de fxc)", name);
    }
    if (FAILED(hrc))
    {
        LOG("[d3d12] error compilando %s: %.400s", name, err ? (const char*)err->GetBufferPointer() : "?");
        CreateDirectoryA("hlsl_err", nullptr);
        char path[128]; snprintf(path, sizeof(path), "hlsl_err/%s.hlsl", name);
        if (FILE* f = fopen(path, "wb")) { fwrite(src.data(), 1, src.size(), f); fclose(f); }
        return nullptr;
    }
    if (cpath[0])
    {
        CreateDirectoryA("shadercache", nullptr);
        char tmp[112]; snprintf(tmp, sizeof(tmp), "%s.%lu", cpath, GetCurrentThreadId());
        HANDLE h = CreateFileA(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            DWORD put = 0; BOOL ok = WriteFile(h, code->GetBufferPointer(), (DWORD)code->GetBufferSize(), &put, nullptr);
            CloseHandle(h);
            if (!ok || !MoveFileExA(tmp, cpath, MOVEFILE_REPLACE_EXISTING)) DeleteFileA(tmp);
        }
    }
    return code;
}


// GOWJ_PROBEBARRIER=1 (diagnostico, sesion 8): cada barrera se graba tambien en una lista de prueba que se cierra al momento;
// si ese Close falla, la barrera es invalida y se registra (sin capa de depuracion es la unica forma de saber cual es)
void ProbedBarrier(UINT n, const D3D12_RESOURCE_BARRIER* bs, int line)
{
    static const bool probe = getenv("GOWJ_PROBEBARRIER") != nullptr;
    if (probe)
    {
        static ComPtr<ID3D12CommandAllocator> pa; static ComPtr<ID3D12GraphicsCommandList> pl;
        if (!pa) g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&pa));
        if (pa)
        {
            pl.Reset(); pa->Reset();
            if (SUCCEEDED(g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, pa.Get(), nullptr, IID_PPV_ARGS(&pl))))
            {
                pl->ResourceBarrier(n, bs);
                HRESULT hr = pl->Close();
                static int logged = 0;
                if (FAILED(hr) && logged++ < 30)
                    for (UINT i = 0; i < n; i++)
                    {
                        const auto& b = bs[i];
                        if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) { LOG("[barrera] INVALIDA linea %d #%u tipo %d", line, i, (int)b.Type); continue; }
                        D3D12_RESOURCE_DESC rd = b.Transition.pResource ? b.Transition.pResource->GetDesc() : D3D12_RESOURCE_DESC{};
                        LOG("[barrera] INVALIDA linea %d #%u/%u: %p %0X -> %0X sub %u (dim %d %llux%u fmt %d flags %X)", line, i, n, (void*)b.Transition.pResource,
                            (unsigned)b.Transition.StateBefore, (unsigned)b.Transition.StateAfter, b.Transition.Subresource, (int)rd.Dimension, (unsigned long long)rd.Width, rd.Height, (int)rd.Format, (unsigned)rd.Flags);
                    }
            }
        }
    }
    g_list->ResourceBarrier(n, bs);
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER br{};
    if (a == b || !r)
    {
        // Transicion invalida (mismo estado antes y despues): invalidaria la lista. Se sustituye por una barrera UAV global (inofensiva).
        static int n = 0;
        if (n++ < 10) LOG("[d3d12] transicion invalida evitada: estado 0x%X -> 0x%X (recurso %p)", (unsigned)a, (unsigned)b, (void*)r);
        br.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; br.UAV.pResource = nullptr;
        return br;
    }
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b };
    return br;
}

// Copia de region comprobada (sesion 23): un rectangulo fuera de los recursos invalida toda la lista de comandos
bool SafeCopyRegion(ID3D12Resource* dst, ID3D12Resource* src, const D3D12_BOX& box, const char* who)
{
    D3D12_RESOURCE_DESC sd = src->GetDesc(), dd = dst->GetDesc();
    UINT w = box.right - box.left, h = box.bottom - box.top;
    bool ok = box.right > box.left && box.bottom > box.top && box.right <= sd.Width && box.bottom <= sd.Height &&
              w <= dd.Width && h <= dd.Height && sd.Format == dd.Format;
    if (!ok)
    {
        static int n = 0;
        if (n++ < 10) LOG("[d3d12] copia invalida evitada (%s): caja (%u,%u)-(%u,%u) origen %llux%u fmt %d -> destino %llux%u fmt %d",
            who, box.left, box.top, box.right, box.bottom, (unsigned long long)sd.Width, sd.Height, (int)sd.Format, (unsigned long long)dd.Width, dd.Height, (int)dd.Format);
        return false;
    }
    D3D12_TEXTURE_COPY_LOCATION dl{ dst, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; dl.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION sl{ src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; sl.SubresourceIndex = 0;
    g_list->CopyTextureRegion(&dl, 0, 0, 0, &sl, &box);
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE Rtv(UINT i) { auto h = g_rtvHeap->GetCPUDescriptorHandleForHeapStart(); h.ptr += i * g_rtvSize; return h; }
D3D12_CPU_DESCRIPTOR_HANDLE SrvCpu(UINT i) { auto h = g_srvHeap->GetCPUDescriptorHandleForHeapStart(); h.ptr += i * g_srvSize; return h; }
D3D12_GPU_DESCRIPTOR_HANDLE SrvGpu(UINT i) { auto h = g_srvHeap->GetGPUDescriptorHandleForHeapStart(); h.ptr += i * g_srvSize; return h; }

ComPtr<ID3D12RootSignature> MakeRootSig(const D3D12_ROOT_SIGNATURE_DESC& d)
{
    ComPtr<ID3DBlob> blob, err; ComPtr<ID3D12RootSignature> rs;
    if (FAILED(D3D12SerializeRootSignature(&d, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) { LOG("[d3d12] firma raiz: %s", err ? (const char*)err->GetBufferPointer() : "?"); return nullptr; }
    g_dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs));
    return rs;
}

// Sesion 24: precompilacion en segundo plano. Al arrancar se traducen y compilan (en varios hilos de baja prioridad) todos los
// shaders ya vistos en partidas anteriores (shaders\*.bin, microcodigo en big endian) para llenar shadercache\. Asi, al entrar en
// una cancion, los shaders salen de la cache en vez de compilarse en el hilo de la GPU (eran ~100 ms cada uno: 12 s de pantalla negra).
// GOWJ_NOPREWARM=1 la desactiva.
// Sustituciones de shaders por hash (sesion 24):
// - Velocidades a cero (desenfoque de movimiento desactivado): los draws de velocidad de personajes/publico (PS DF75D55E,
//   99CD25F5) y de camara (DF757D71) daban desplazamientos de 16+ pixeles con los personajes casi quietos y el compositor
//   C591749F emborronaba toda la imagen. Con velocidad 0 la imagen coincide con la de la 360 (personajes nitidos, fondo
//   desenfocado por profundidad, halo). GOWJ_MOTIONBLUR=1 lo deja como antes. Pendiente: la causa real de esas velocidades.
// - GOWJ_PSOUT=<hash>:<expresion>[;...] (diagnostico): sustituye la salida oC0 de esos pixel shaders.
bool ApplyShaderOverrides(uint32_t type, uint64_t hash, std::string& hlsl)
{
    if (type != 1) return false;
    bool changed = false;
    size_t r = hlsl.rfind("    return XVte.w");
    if (r == std::string::npos) return false;
    // Sesion 24: grano de pelicula del ultimo pase (0261F529: lerp(color, ruido, 0.08); el ruido mezcla una textura 64x64 con
    // frac(668.3*u + 270700*v)). Aqui sale como rayas verticales finas sobre los personajes. GOWJ_NOGRAIN=1 (grano_pelicula=0 en
    // gowj.ini, por defecto) lo quita: el pase devuelve el color sin el ruido.
    {
        static const bool noGrain = getenv("GOWJ_NOGRAIN") && atoi(getenv("GOWJ_NOGRAIN")) != 0;
        if (noGrain && hash == 0x0261F5295D681495ull)
        { hlsl.insert(r, "    oC0 = float4(r[0].xyz, oC0.w);\n"); changed = true; r = hlsl.rfind("    return XVte.w"); }
    }
    // GoWJ sesion 10: FXAA de consola del juego (PS 0BA717F8: luma en alfa, 4 esquinas, ramas con predicado). Traducido rompia el
    // texto y el HUD (letras hechas de trozos de otras). Se devuelve el color del pixel sin suavizar hasta encontrar el fallo de la
    // traduccion. Diagnostico: GOWJ_NOGAMEFXAA=1 lo quita (se vio que el texto roto es intermitente y no depende de esto).
    {
        static const bool noGameFxaa = getenv("GOWJ_NOGAMEFXAA") != nullptr;   // desactivado por defecto: el texto roto no venia de aqui
        if (noGameFxaa && hash == 0x0BA717F87EEF07F8ull)
        { hlsl.insert(r, "    oC0 = float4(XTex[0].SampleLevel(XSampP, i.t[0].xy, 0).xyz, 1);\n"); changed = true; r = hlsl.rfind("    return XVte.w"); }
    }
    // Sesion 24: con la profundidad restaurada (aliasing color->profundidad) las velocidades ya son correctas: desenfoque de
    // movimiento activo por defecto. GOWJ_NOMOTIONBLUR=1 (o desenfoque_movimiento=0 en gowj.ini) lo quita.
    static const bool motionBlur = getenv("GOWJ_NOMOTIONBLUR") == nullptr;
    if (!motionBlur && (hash == 0xDF757D719DE4CB8Full || hash == 0xDF75D55E88917A96ull || hash == 0x99CD25F562821A21ull))
    { hlsl.insert(r, "    oC0 = float4(0, 0, 0, 0);\n"); changed = true; r = hlsl.rfind("    return XVte.w"); }
    if (const char* nd = getenv("GOWJ_NODISCARD"))   // diagnostico: quita los discard (todos con "1", o los PS de la lista)
    {
        bool all = strcmp(nd, "1") == 0;
        if (!all) for (const char* s = nd; s && *s; ) { if (strtoull(s, nullptr, 16) == hash) { all = true; break; } s = strchr(s, ','); if (s) s++; }
        if (all) for (size_t k; (k = hlsl.find(" discard;")) != std::string::npos; ) { hlsl.replace(k, 9, " {}"); changed = true; }
        r = hlsl.rfind("    return XVte.w");
    }
    if (const char* env = getenv("GOWJ_PSOUT"))
    {
        std::string all = env;
        for (size_t pos = 0; pos <= all.size(); )
        {
            size_t end = all.find(';', pos); if (end == std::string::npos) end = all.size();
            std::string spec = all.substr(pos, end - pos); pos = end + 1;
            size_t c = spec.find(':');
            if (c != std::string::npos && strtoull(spec.substr(0, c).c_str(), nullptr, 16) == hash)
            { hlsl.insert(r, std::string(getenv("GOWJ_PSOUTAT") ? "    if (XVte.z < 0) " : "    ") + "oC0 = " + spec.substr(c + 1) + ";\n"); changed = true; r = hlsl.rfind("    return XVte.w"); }
        }
    }
    return changed;
}


// Sesion 25: precreacion de PSO. Cada PSO nuevo se apunta en shadercache/psolist.bin (descripcion + huella de los shaders); en el
// siguiente arranque, tras precompilar los shaders, se crean todos en segundo plano y el draw los toma ya hechos (en los cortes de
// escena el juego pedia ~60 PSO nuevos de golpe: ~50 ms de tiron). Solo se usa uno si los shaders compilados coinciden byte a byte
// (huella FNV del bytecode). GOWJ_NOPSOPREWARM=1 lo desactiva.
struct PsoRec
{
    uint32_t magic = 0x50534F31; uint32_t rect = 0; uint64_t key = 0, vsHash = 0, psHash = 0, vsBc = 0, psBc = 0;
    D3D12_BLEND_DESC blend{}; D3D12_RASTERIZER_DESC rast{}; D3D12_DEPTH_STENCIL_DESC ds{}; DXGI_FORMAT dsv{}; D3D12_PRIMITIVE_TOPOLOGY_TYPE topo{};
    UINT numRt = 0; DXGI_FORMAT rtv[2]{}; UINT sampleMask = 0;
};
static uint64_t BcHash(const void* p, size_t n) { uint64_t h = 1469598103934665603ull; for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t*)p)[i]) * 1099511628211ull; return h; }
static std::mutex g_psoPreMutex; static std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> g_psoPre; static std::unordered_set<uint64_t> g_psoListed;
static std::atomic<bool> g_psoListLoaded{ false };
static void RecordPso(uint64_t key, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& pd, uint64_t vsHash, uint64_t psHash, int rect)
{
    static const bool off = getenv("GOWJ_NOPSOPREWARM") != nullptr || getenv("GOWJ_NOSHADERCACHE") != nullptr;
    if (off || !g_psoListLoaded) return;
    { std::lock_guard lk(g_psoPreMutex); if (!g_psoListed.insert(key).second) return; }
    PsoRec r; r.rect = rect; r.key = key; r.vsHash = vsHash; r.psHash = psHash;
    r.vsBc = BcHash(pd.VS.pShaderBytecode, pd.VS.BytecodeLength); r.psBc = BcHash(pd.PS.pShaderBytecode, pd.PS.BytecodeLength);
    r.blend = pd.BlendState; r.rast = pd.RasterizerState; r.ds = pd.DepthStencilState; r.dsv = pd.DSVFormat; r.topo = pd.PrimitiveTopologyType;
    r.numRt = pd.NumRenderTargets; r.rtv[0] = pd.RTVFormats[0]; r.rtv[1] = pd.RTVFormats[1]; r.sampleMask = pd.SampleMask;
    static std::mutex fileMutex; std::lock_guard fl(fileMutex);
    CreateDirectoryA("shadercache", nullptr);
    if (FILE* f = fopen("shadercache/psolist.bin", "ab")) { fwrite(&r, sizeof(r), 1, f); fclose(f); }
}
static bool TakePrewarmedPso(uint64_t key, ComPtr<ID3D12PipelineState>& out)
{
    std::lock_guard lk(g_psoPreMutex);
    auto it = g_psoPre.find(key); if (it == g_psoPre.end()) return false;
    out = std::move(it->second); g_psoPre.erase(it); return true;
}
// Sesion 27: creacion asincrona de PSO. En una instalacion nueva (sin shadercache/psolist.bin ni cache del controlador) cada PSO
// costaba 40-200 ms en el hilo de la GPU: 29 PSO en un corte = 1,2 s de congelacion. Ahora el draw encola el PSO, se omite ese
// frame y lo toma ya hecho (TakePrewarmedPso) en cuanto esta. GOWJ_SYNCPSO=1 = crear en el momento como antes.
static std::unordered_set<uint64_t> g_psoPending, g_psoFailed;
struct PsoJob { uint64_t key; D3D12_GRAPHICS_PIPELINE_STATE_DESC pd; uint64_t vs, ps; int gs; };
static void RecordPso(uint64_t key, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& pd, uint64_t vsHash, uint64_t psHash, int rect);
static bool EnqueuePso(const PsoJob& job)   // false si ya fallo antes (no se reintenta)
{
    static std::mutex m; static std::condition_variable cv; static std::vector<PsoJob> q; static std::once_flag once;
    {
        std::lock_guard lk(g_psoPreMutex);
        if (g_psoFailed.count(job.key)) return false;
        if (!g_psoPending.insert(job.key).second) return true;   // ya en cola
    }
    std::call_once(once, [] {
        unsigned n = std::max(2u, std::min(4u, std::thread::hardware_concurrency() / 3));
        for (unsigned i = 0; i < n; i++)
            std::thread([] {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                for (;;)
                {
                    PsoJob j;
                    { std::unique_lock<std::mutex> l(m); cv.wait(l, [] { return !q.empty(); }); j = q.back(); q.pop_back(); }
                    ComPtr<ID3D12PipelineState> pso;
                    HRESULT hr = g_dev->CreateGraphicsPipelineState(&j.pd, IID_PPV_ARGS(&pso));
                    if (SUCCEEDED(hr)) RecordPso(j.key, j.pd, j.vs, j.ps, j.gs);
                    std::lock_guard lk(g_psoPreMutex);
                    if (SUCCEEDED(hr)) g_psoPre[j.key] = pso;
                    else { g_psoFailed.insert(j.key); static int nf = 0; if (nf++ < 12) LOG("[d3d12] PSO asincrono fallido hr=%08X VS %016llX PS %016llX", (unsigned)hr, (unsigned long long)j.vs, (unsigned long long)j.ps); }
                    g_psoPending.erase(j.key);
                }
            }).detach();
    });
    { std::lock_guard<std::mutex> l(m); q.push_back(job); }
    cv.notify_one();
    return true;
}
static void PrewarmPsos(const std::unordered_map<uint64_t, ComPtr<ID3DBlob>>& blobs)
{
    std::vector<PsoRec> recs;
    if (FILE* f = fopen("shadercache/psolist.bin", "rb")) { PsoRec r; while (fread(&r, sizeof(r), 1, f) == 1) if (r.magic == 0x50534F31) recs.push_back(r); fclose(f); }
    // GoWJ sesion 11: solo cuentan como "ya guardadas" las entradas cuyos shaders coinciden con los actuales. Antes las viejas (de
    // versiones anteriores del HLSL) bloqueaban para siempre el registro de la nueva: ~3.000 PSO se creaban en plena partida en cada
    // sesion (tirones al cambiar de zona). Al terminar se reescribe la lista sin las entradas viejas.
    if (recs.empty()) { g_psoListLoaded = true; return; }
    std::vector<char> valid(recs.size(), 0);
    double t0 = QpcMs(); std::atomic<size_t> next{ 0 }; std::atomic<uint32_t> ok{ 0 }, miss{ 0 };
    auto worker = [&]() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        for (;;)
        {
            size_t i = next++; if (i >= recs.size()) break;
            const PsoRec& r = recs[i];
            auto v = blobs.find(r.vsHash), p = blobs.find(r.psHash);
            if (v == blobs.end() || p == blobs.end() || BcHash(v->second->GetBufferPointer(), v->second->GetBufferSize()) != r.vsBc
                || BcHash(p->second->GetBufferPointer(), p->second->GetBufferSize()) != r.psBc) { miss++; continue; }
            valid[i] = 1;
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature = g_drawRootSig.Get();
            pd.VS = { v->second->GetBufferPointer(), v->second->GetBufferSize() }; pd.PS = { p->second->GetBufferPointer(), p->second->GetBufferSize() };
            if (r.rect) { ID3DBlob* gb = r.rect == 1 ? g_rectGs.Get() : r.rect == 2 ? g_uiGs.Get() : g_rectGsPano.Get(); if (!gb) { miss++; continue; } pd.GS = { gb->GetBufferPointer(), gb->GetBufferSize() }; }
            pd.BlendState = r.blend; pd.RasterizerState = r.rast; pd.DepthStencilState = r.ds; pd.DSVFormat = r.dsv; pd.PrimitiveTopologyType = r.topo;
            pd.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF; pd.SampleMask = r.sampleMask; pd.SampleDesc.Count = 1;
            pd.NumRenderTargets = r.numRt; pd.RTVFormats[0] = r.rtv[0]; pd.RTVFormats[1] = r.rtv[1];
            ComPtr<ID3D12PipelineState> pso;
            if (SUCCEEDED(g_dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)))) { std::lock_guard lk(g_psoPreMutex); g_psoPre[r.key] = pso; ok++; }
        }
    };
    unsigned hw = std::thread::hardware_concurrency(); unsigned n = hw > 3 ? std::min(hw - 2, 6u) : 1u;
    std::vector<std::thread> pool; for (unsigned k = 0; k < n; k++) pool.emplace_back(worker); for (auto& th : pool) th.join();
    LOG("[d3d12] PSO precreados: %u de %zu en %.1f s (%u sin shaders iguales)", ok.load(), recs.size(), (QpcMs() - t0) / 1000.0, miss.load());
    {
        std::lock_guard lk(g_psoPreMutex);
        for (size_t i = 0; i < recs.size(); i++) if (valid[i]) g_psoListed.insert(recs[i].key);
        if (miss) if (FILE* f = fopen("shadercache/psolist.bin", "wb"))
        {
            std::unordered_set<uint64_t> seen;
            for (size_t i = 0; i < recs.size(); i++) if (valid[i] && seen.insert(recs[i].key).second) fwrite(&recs[i], sizeof(PsoRec), 1, f);
            fclose(f);
            LOG("[d3d12] lista de PSO limpiada: %zu entradas validas, %u viejas quitadas", seen.size(), miss.load());
        }
    }
    g_psoListLoaded = true;
}

void PrewarmShaders()
{
    if (getenv("GOWJ_NOPREWARM") || getenv("GOWJ_NOSHADERCACHE")) { g_psoListLoaded = getenv("GOWJ_NOSHADERCACHE") == nullptr; return; }
    std::vector<std::string> files;
    WIN32_FIND_DATAA fd; HANDLE fh = FindFirstFileA("shaders/*.bin", &fd);
    if (fh == INVALID_HANDLE_VALUE) { g_psoListLoaded = true; return; }
    do { if ((fd.cFileName[0] == 'v' || fd.cFileName[0] == 'p') && fd.cFileName[1] == 's') files.push_back(fd.cFileName); } while (FindNextFileA(fh, &fd));
    FindClose(fh);
    if (files.empty()) { g_psoListLoaded = true; return; }
    std::thread([files]() {
        double t0 = QpcMs();
        std::mutex blobMutex; std::unordered_map<uint64_t, ComPtr<ID3DBlob>> blobs;
        unsigned hw = std::thread::hardware_concurrency();
        unsigned n = hw > 3 ? std::min(hw - 2, 8u) : 1u;
        std::atomic<size_t> next{ 0 }; std::atomic<uint32_t> ok{ 0 };
        auto worker = [&]() {
            t_prewarm = true;
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            for (;;)
            {
                size_t i = next++;
                if (i >= files.size()) break;
                const std::string& fn = files[i];
                std::string path = "shaders/" + fn;
                FILE* f = fopen(path.c_str(), "rb");
                if (!f) continue;
                std::vector<uint32_t> w;
                uint32_t be;
                while (fread(&be, 4, 1, f) == 1) w.push_back(__builtin_bswap32(be));
                fclose(f);
                if (w.empty()) continue;
                uint32_t type = fn[0] == 'p' ? 1 : 0;
                std::string name = fn.substr(0, fn.size() - 4);
                TranslatedShader t;
                { std::lock_guard<std::mutex> lk(g_xlatMutex); t = TranslateXenosShader(type, w.data(), (uint32_t)w.size()); }
                if (!t.ok) continue;
                uint64_t hash = strtoull(name.c_str() + 3, nullptr, 16);
                ApplyShaderOverrides(type, hash, t.hlsl);
                auto blob = Compile(t.hlsl, "main", type ? "ps_5_0" : "vs_5_0", name.c_str());
                if (!blob && type == 1 && t.hlsl.find("SampleGrad") != std::string::npos)
                {
                    { std::lock_guard<std::mutex> lk(g_xlatMutex); g_xlatNoGrad = true; t = TranslateXenosShader(type, w.data(), (uint32_t)w.size()); g_xlatNoGrad = false; }
                    if (t.ok) { ApplyShaderOverrides(type, hash, t.hlsl); blob = Compile(t.hlsl, "main", "ps_5_0", name.c_str()); }
                }
                if (blob) { ok++; std::lock_guard lk(blobMutex); blobs[hash] = blob; }
            }
        };
        std::vector<std::thread> pool;
        for (unsigned k = 0; k < n; k++) pool.emplace_back(worker);
        for (auto& th : pool) th.join();
        LOG("[d3d12] precompilacion: %u/%zu shaders listos en %.1f s (%u hilos)", ok.load(), files.size(), (QpcMs() - t0) / 1000.0, n);
        if (getenv("GOWJ_NOPSOPREWARM")) { g_psoListLoaded = true; return; }
        PrewarmPsos(blobs);
    }).detach();
}

// Volcado DRED: ultimas operaciones de la GPU y fallo de pagina (con GOWJ_DRED=1)
uint64_t g_drawSerial = 0;
static void DumpDred()
{
    ComPtr<ID3D12DeviceRemovedExtendedData> dred;
    if (FAILED(g_dev.As(&dred))) { LOG("[dred] no disponible"); return; }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bo{};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&bo)))
        for (const D3D12_AUTO_BREADCRUMB_NODE* nd = bo.pHeadAutoBreadcrumbNode; nd; nd = nd->pNext)
        {
            uint32_t done = nd->pLastBreadcrumbValue ? *nd->pLastBreadcrumbValue : 0;
            LOG("[dred] nodo lista %p cola %p: %u/%u", (void*)nd->pCommandList, (void*)nd->pCommandQueue, done, nd->BreadcrumbCount);
            if (done >= nd->BreadcrumbCount) continue;   // lista terminada
            LOG("[dred] lista %p: %u de %u operaciones completadas", (void*)nd->pCommandList, done, nd->BreadcrumbCount);
            uint32_t draws = 0; for (uint32_t i = 0; i < done && i < nd->BreadcrumbCount; i++) if (nd->pCommandHistory[i] == D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED || nd->pCommandHistory[i] == D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED) draws++;
            LOG("[dred]   draws completados en la lista: %u", draws);
            for (uint32_t i = done > 6 ? done - 6 : 0; i < nd->BreadcrumbCount && i < done + 6; i++) LOG("[dred]   %s op %u = %d", i == done ? ">>" : "  ", i, (int)nd->pCommandHistory[i]);
        }
    D3D12_DRED_PAGE_FAULT_OUTPUT pf{};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pf)))
    {
        LOG("[dred] fallo de pagina en VA %016llX", (unsigned long long)pf.PageFaultVA);
        int n = 0;
        for (auto* a = pf.pHeadExistingAllocationNode; a && n < 10; a = a->pNext, n++) LOG("[dred]   recurso vivo cercano: tipo %d %s", (int)a->AllocationType, a->ObjectNameA ? a->ObjectNameA : "");
        n = 0;
        for (auto* a = pf.pHeadRecentFreedAllocationNode; a && n < 10; a = a->pNext, n++) LOG("[dred]   recurso LIBERADO hace poco: tipo %d %s", (int)a->AllocationType, a->ObjectNameA ? a->ObjectNameA : "");
    }
    for (int i = 0; i < kFrames; i++) if (g_ring[i]) LOG("[dred] anillo %d: VA %016llX tamano %llu", i, (unsigned long long)g_ring[i]->GetGPUVirtualAddress(), (unsigned long long)g_ring[i]->GetDesc().Width);
}

bool Init(HWND hwnd)
{
    if (getenv("GOWJ_D3D12_DEBUG")) { ComPtr<ID3D12Debug> dbg; if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) dbg->EnableDebugLayer(); }
    if (getenv("GOWJ_DRED"))   // GOWJ_DRED=1: rastro de la GPU (DRED) para diagnosticar dispositivos colgados (sesion 7 de NG2)
    {
        ComPtr<ID3D12DeviceRemovedExtendedDataSettings> ds;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&ds)))) { ds->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON); ds->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON); }
    }
    ComPtr<IDXGIFactory4> factory;
    if (!Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1")) return false;
    if (!Check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_dev)), "D3D12CreateDevice")) return false;
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (!Check(g_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue)), "CreateCommandQueue")) return false;
    DXGI_SWAP_CHAIN_DESC1 sd{}; sd.Width = kRtW; sd.Height = kRtH; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = kFrames; sd.Scaling = DXGI_SCALING_STRETCH; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    // Sesion 24 (retraso de video): latencia maxima de frames configurable (GOWJ_FRAMELATENCY=1/2; por defecto 0 = sin limite, como antes)
    static const int frameLatency = getenv("GOWJ_FRAMELATENCY") ? atoi(getenv("GOWJ_FRAMELATENCY")) : 0;   // 1-2 bajan los fps (medido: 48 y 55 fps)
    if (frameLatency > 0) sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    g_swapFlags = sd.Flags;
    ComPtr<IDXGISwapChain1> sc1;
    if (!Check(factory->CreateSwapChainForHwnd(g_queue.Get(), hwnd, &sd, nullptr, nullptr, &sc1), "CreateSwapChainForHwnd")) return false;
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    sc1.As(&g_swap);
    g_presentHwnd = hwnd;
    if (frameLatency > 0 && g_swap)
    {
        g_swap->SetMaximumFrameLatency((UINT)frameLatency);
        g_frameWait = g_swap->GetFrameLatencyWaitableObject();
        LOG("[d3d12] latencia maxima de presentacion: %d frame(s)", frameLatency);
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = kFrames + 1 + 64;
    g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rtvHeap));
    g_rtvSize = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT i = 0; i < kFrames; i++)
    {
        g_swap->GetBuffer(i, IID_PPV_ARGS(&g_backbuffers[i]));
        g_dev->CreateRenderTargetView(g_backbuffers[i].Get(), nullptr, Rtv(i));
        g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc[i]));
    }
    D3D12_DESCRIPTOR_HEAP_DESC dh{}; dh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; dh.NumDescriptors = 64;
    g_dev->CreateDescriptorHeap(&dh, IID_PPV_ARGS(&g_dsvHeap));
    g_dsvSize = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    D3D12_DESCRIPTOR_HEAP_DESC sh{}; sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; sh.NumDescriptors = 32 + kFrames * kDescPerFrame; sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    g_dev->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&g_srvHeap));
    g_srvSize = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[0].Get(), nullptr, IID_PPV_ARGS(&g_list));
    g_list->Close();
    g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
    g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    D3D12_STATIC_SAMPLER_DESC samp{}; samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP; samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL; samp.MaxLOD = D3D12_FLOAT32_MAX;
    // Blit: tabla con 1 SRV
    D3D12_DESCRIPTOR_RANGE r0{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
    D3D12_ROOT_PARAMETER bp{}; bp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; bp.DescriptorTable = { 1, &r0 }; bp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    g_blitRootSig = MakeRootSig({ 1, &bp, 1, &samp, D3D12_ROOT_SIGNATURE_FLAG_NONE });
    // Draws: CBV VS (b0), CBV PS (b0), SRV crudo de vertices (t0, VS), tabla t1..t16 (PS)
    D3D12_DESCRIPTOR_RANGE r1{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 48, 1, 0, 0 };   // t1-t16: 2D, t17-t32: 3D, t33-t48: cubo (sesion 21)
    D3D12_ROOT_PARAMETER dp[4]{};
    dp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; dp[0].Descriptor = { 0, 0 }; dp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    dp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; dp[1].Descriptor = { 0, 0 }; dp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    dp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; dp[2].Descriptor = { 0, 0 }; dp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    dp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; dp[3].DescriptorTable = { 1, &r1 }; dp[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;   // GoWJ: el VS tambien lee texturas
    // Sesion 24: s0 = lineal y s1 = punto, ambos repitiendo; el modo de borde de cada textura (repetir, espejo, recortar,
    // color de borde) se emula en el shader con XTexMode (constante de fetch: clamp_x/y/z, filtros, color de borde)
    D3D12_STATIC_SAMPLER_DESC ds[2] = { samp, samp };
    ds[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;   // GoWJ: tfetch en el VS
    ds[0].AddressU = ds[0].AddressV = ds[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    ds[1] = ds[0]; ds[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT; ds[1].ShaderRegister = 1;
    {   // Sesion 23: filtrado anisotropo en s0 (GOWJ_ANISO = 0/2/4/8/16, por defecto 16; anisotropico= en gowj.ini). En pases a
        // pantalla completa los gradientes son minimos y equivale al lineal; mejora suelos y paredes vistos de lado.
        const char* a = getenv("GOWJ_ANISO"); int an = a ? atoi(a) : 16;
        if (an >= 2) { ds[0].Filter = D3D12_FILTER_ANISOTROPIC; ds[0].MaxAnisotropy = (UINT)std::min(an, 16); }
        LOG("[d3d12] filtrado de texturas: %s (x%d)", an >= 2 ? "anisotropo" : "lineal", an >= 2 ? std::min(an, 16) : 1);
    }
    // GoWJ sesion 4: dp[4] = UAV u0 para memexport (registros que escribe el VS)
    D3D12_ROOT_PARAMETER dp5[5]; for (int i = 0; i < 4; i++) dp5[i] = dp[i];
    dp5[4] = {}; dp5[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; dp5[4].Descriptor = { 0, 0 }; dp5[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    g_drawRootSig = MakeRootSig({ 5, dp5, 2, ds, D3D12_ROOT_SIGNATURE_FLAG_NONE });
    if (!g_blitRootSig || !g_drawRootSig) { g_failed = true; return false; }

    // GOWJ_UPSCALE=bilineal: escalado bilineal (antes); por defecto Catmull-Rom
    static const bool bilinear = getenv("GOWJ_UPSCALE") && !strcmp(getenv("GOWJ_UPSCALE"), "bilineal");
    // GoWJ sesion 11: GOWJ_NITIDEZ=<0..1> (nitidez= en gowj.ini; 0 = sin enfoque). Por defecto 0,5.
    static const float sharp = std::clamp(getenv("GOWJ_NITIDEZ") ? (float)atof(getenv("GOWJ_NITIDEZ")) : 0.5f, 0.0f, 1.0f);
    char sharpDef[64]; snprintf(sharpDef, sizeof(sharpDef), sharp > 0 ? "#define SHARPEN 1\n#define SHARP %.3f\n" : "#define SHARP %.3f\n", sharp);
    // GoWJ sesion 11: brillo (GOWJ_BRILLO / brillo= en gowj.ini; gamma final, 1,0 = consola). Se probo a oscurecer 32:9 por defecto
    // (0,9) creyendo que salia sobreexpuesto, pero con la misma escena el centro es identico a 16:9: la diferencia de media venia de la
    // vineta del juego, que a 32:9 cae en los extremos del monitor. GOWJ_BRILLOPANO=<g> = gamma solo para la partida panoramica.
    const char* brS = getenv("GOWJ_BRILLO");
    float brUser = brS ? (float)atof(brS) : 1.0f; if (!(brUser >= 0.5f && brUser <= 2.0f)) brUser = 1.0f;
    float brScene = getenv("GOWJ_BRILLOPANO") && PanoAspect() > 16.0f / 9.0f + 1e-3f ? std::clamp((float)atof(getenv("GOWJ_BRILLOPANO")), 0.5f, 2.0f) : brUser;
    auto gammaDef = [](float g) { char d[48]; snprintf(d, sizeof(d), "#define PORT_GAMMA %.3f\n", g); return std::string(d); };
    const std::string blitSrc = std::string(sharpDef) + gammaDef(brUser) + kBlitHlsl;
    const std::string blitSrcScene = std::string(sharpDef) + gammaDef(brScene) + kBlitHlsl;
    auto vs = Compile(blitSrc.c_str(), "vs_main", "vs_5_0", "blit"), ps = Compile(blitSrc.c_str(), bilinear ? "ps_main" : "ps_cubic", "ps_5_0", "blit");
    auto psScene = brScene != brUser ? Compile(blitSrcScene.c_str(), bilinear ? "ps_main" : "ps_cubic", "ps_5_0", "blit") : nullptr;
    LOG("[d3d12] brillo %.2f (partida %.2f)", brUser, brScene);
    LOG("[d3d12] escalado %s, nitidez %.2f", bilinear ? "bilineal" : "Catmull-Rom", sharp);
    g_rectGs = Compile(std::string(kXenosInterfaceHlsl) + kRectGsHlsl, "main", "gs_5_0", "rectlist_gs");
    if (PanoAspect() > 16.0f / 9.0f + 1e-3f && !getenv("GOWJ_NOPANOUI"))   // GoWJ sesion 11: GOWJ_NOPANOUI=1 = interfaz sin comprimir (diagnostico)
    {   // sesion 25: GS que comprimen la interfaz 2D hacia el centro con panoramico
        char def[128]; snprintf(def, sizeof(def), "#define KX %.6f\n%s%s", PanoAspect() / (16.0f / 9.0f), getenv("GOWJ_PANOOVERSCAN") ? "#define PANO_NO_OVERSCAN 1\n" : "", getenv("GOWJ_PANOWSTRICT") ? "#define PANO_WSTRICT 1\n" : "");
        g_uiGs = Compile(std::string(def) + kXenosInterfaceHlsl + kPanoUiFnHlsl + kUiGsHlsl, "main", "gs_5_0", "pano_ui_gs");
        g_rectGsPano = Compile(std::string(def) + kXenosInterfaceHlsl + kPanoUiFnHlsl + kRectGsPanoHlsl, "main", "gs_5_0", "pano_rect_gs");
        LOG("[d3d12] panoramico: GS de la interfaz %s", g_uiGs && g_rectGsPano ? "listos" : "NO compilados");
    }
    if (!vs || !ps) { g_failed = true; return false; }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_blitRootSig.Get();
    pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() }; pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = 0xFFFFFFFF;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM; pd.SampleDesc.Count = 1;
    if (!Check(g_dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_blitPso)), "CreateGraphicsPipelineState")) return false;
    if (psScene) { D3D12_GRAPHICS_PIPELINE_STATE_DESC sd3 = pd; sd3.PS = { psScene->GetBufferPointer(), psScene->GetBufferSize() }; g_dev->CreateGraphicsPipelineState(&sd3, IID_PPV_ARGS(&g_blitPsoScene)); }
    if (auto ops = Compile(kBlitHlsl, "ps_overlay", "ps_5_0", "overlay"))   // sesion 25: contador en pantalla
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC od = pd; od.PS = { ops->GetBufferPointer(), ops->GetBufferSize() };
        auto& bt = od.BlendState.RenderTarget[0];
        bt.BlendEnable = TRUE; bt.SrcBlend = D3D12_BLEND_ONE; bt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA; bt.BlendOp = D3D12_BLEND_OP_ADD;
        bt.SrcBlendAlpha = D3D12_BLEND_ONE; bt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA; bt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        g_dev->CreateGraphicsPipelineState(&od, IID_PPV_ARGS(&g_overlayPso));
    }
    if (PanoAspect() > 16.0f / 9.0f + 1e-3f)   // GoWJ sesion 11: laterales difuminados de las pantallas a 16:9 (GOWJ_BORDES=negro = barras negras)
    {
        const char* bo = getenv("GOWJ_BORDES");
        if (!bo || _stricmp(bo, "negro"))
        {
            D3D12_DESCRIPTOR_RANGE rs{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
            D3D12_ROOT_PARAMETER rp[2]{};
            rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[0].DescriptorTable = { 1, &rs }; rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[1].Constants = { 0, 0, 4 }; rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            g_sideRootSig = MakeRootSig({ 2, rp, 1, &samp, D3D12_ROOT_SIGNATURE_FLAG_NONE });
            auto svs = Compile(kSideHlsl, "vs_main", "vs_5_0", "side"), sps = Compile(kSideHlsl, "ps_main", "ps_5_0", "side");
            if (g_sideRootSig && svs && sps)
            {
                D3D12_GRAPHICS_PIPELINE_STATE_DESC sd2 = pd; sd2.pRootSignature = g_sideRootSig.Get();
                sd2.VS = { svs->GetBufferPointer(), svs->GetBufferSize() }; sd2.PS = { sps->GetBufferPointer(), sps->GetBufferSize() };
                g_dev->CreateGraphicsPipelineState(&sd2, IID_PPV_ARGS(&g_sidePso));
            }
            LOG("[d3d12] laterales difuminados en pantallas 16:9: %s", g_sidePso ? "si" : "NO (fallo al compilar)");
        }
    }
    // Copia de profundidad por GPU (sesion 21): lee el plano de profundidad y escribe R32_FLOAT + RGBA8 con los bits 24_8 empaquetados
    {
        D3D12_DESCRIPTOR_RANGE rd{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
        D3D12_ROOT_PARAMETER rp[2]{};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[0].Constants = { 0, 0, 4 }; rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[1].DescriptorTable = { 1, &rd }; rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        g_depthCopyRootSig = MakeRootSig({ 2, rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE });
        auto dvs = Compile(kDepthCopyHlsl, "vs_main", "vs_5_0", "depthcopy"), dps = Compile(kDepthCopyHlsl, "ps_main", "ps_5_0", "depthcopy");
        if (g_depthCopyRootSig && dvs && dps)
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC dpd{};
            dpd.pRootSignature = g_depthCopyRootSig.Get();
            dpd.VS = { dvs->GetBufferPointer(), dvs->GetBufferSize() }; dpd.PS = { dps->GetBufferPointer(), dps->GetBufferSize() };
            dpd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            dpd.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            dpd.BlendState.IndependentBlendEnable = TRUE;
            dpd.SampleMask = 0xFFFFFFFF;
            dpd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; dpd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            dpd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            dpd.NumRenderTargets = 2; dpd.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT; dpd.RTVFormats[1] = DXGI_FORMAT_R8G8B8A8_UNORM; dpd.SampleDesc.Count = 1;
            g_dev->CreateGraphicsPipelineState(&dpd, IID_PPV_ARGS(&g_depthCopyPso));
        }
        D3D12_DESCRIPTOR_HEAP_DESC rh{}; rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rh.NumDescriptors = 64;
        g_dev->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&g_rtvHeap2));
    }
    {   // Resolve de color con MSAA por GPU (sesion 24)
        D3D12_DESCRIPTOR_RANGE rd{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
        D3D12_ROOT_PARAMETER rp[2]{};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[0].Constants = { 0, 0, 8 }; rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[1].DescriptorTable = { 1, &rd }; rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        g_colorResolveRootSig = MakeRootSig({ 2, rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE });
        auto cvs = Compile(kColorResolveHlsl, "vs_main", "vs_5_0", "colorresolve"), cps = Compile(kColorResolveHlsl, "ps_main", "ps_5_0", "colorresolve");
        if (g_colorResolveRootSig && cvs && cps)
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC cpd{};
            cpd.pRootSignature = g_colorResolveRootSig.Get();
            cpd.VS = { cvs->GetBufferPointer(), cvs->GetBufferSize() }; cpd.PS = { cps->GetBufferPointer(), cps->GetBufferSize() };
            cpd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            cpd.SampleMask = 0xFFFFFFFF;
            cpd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; cpd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            cpd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            cpd.NumRenderTargets = 1; cpd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM; cpd.SampleDesc.Count = 1;
            g_dev->CreateGraphicsPipelineState(&cpd, IID_PPV_ARGS(&g_colorResolvePso));
            cpd.RTVFormats[0] = DXGI_FORMAT_R16G16_UNORM;
            g_dev->CreateGraphicsPipelineState(&cpd, IID_PPV_ARGS(&g_colorResolvePso1616));
            cpd.RTVFormats[0] = DXGI_FORMAT_R16G16_FLOAT;
            g_dev->CreateGraphicsPipelineState(&cpd, IID_PPV_ARGS(&g_colorResolvePso1616F));
            cpd.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;   // sesion 16 de NG2: escena HDR -> k_16_16_16_16_FLOAT
            g_dev->CreateGraphicsPipelineState(&cpd, IID_PPV_ARGS(&g_colorResolvePso16F));
            cpd.RTVFormats[0] = DXGI_FORMAT_B5G6R5_UNORM;   // sesion 19 de NG2: 8888 -> k_5_6_5 por GPU
            if (FAILED(g_dev->CreateGraphicsPipelineState(&cpd, IID_PPV_ARGS(&g_colorResolvePso565)))) LOG("[d3d12] sin PSO de resolve a 5_6_5");
            cpd.RTVFormats[0] = DXGI_FORMAT_R10G10B10A2_UNORM;   // GoWJ sesion 8: escena HDR -> bits 7e3 por GPU
            if (FAILED(g_dev->CreateGraphicsPipelineState(&cpd, IID_PPV_ARGS(&g_colorResolvePso7e3)))) LOG("[d3d12] sin PSO de resolve 7e3");
            cpd.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_UNORM;   // GoWJ sesion 8: posproceso -> k_16_16_16_16 por GPU
            if (FAILED(g_dev->CreateGraphicsPipelineState(&cpd, IID_PPV_ARGS(&g_colorResolvePsoU16)))) LOG("[d3d12] sin PSO de resolve 16_16_16_16");
        }
        D3D12_DESCRIPTOR_HEAP_DESC rh{}; rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rh.NumDescriptors = kRtvHeap3Size;
        g_dev->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&g_rtvHeap3));
    }
    {   // Aliasing color -> profundidad (sesion 24)
        D3D12_DESCRIPTOR_RANGE rd{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
        D3D12_ROOT_PARAMETER rp[1]{};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[0].DescriptorTable = { 1, &rd }; rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        g_colorToDepthRootSig = MakeRootSig({ 1, rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE });
        // GOWJ_NOSWAP40=1 (diagnostico): sin el intercambio de columnas de 40 muestras (antes)
        const std::string c2dSrc = std::string(getenv("GOWJ_NOSWAP40") ? "#define NO_SWAP40 1\n" : "") + kColorToDepthHlsl;
        auto zvs = Compile(c2dSrc, "vs_main", "vs_5_0", "color2depth"), zps = Compile(c2dSrc, "ps_main", "ps_5_0", "color2depth");
        if (g_colorToDepthRootSig && zvs && zps)
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC zpd{};
            zpd.pRootSignature = g_colorToDepthRootSig.Get();
            zpd.VS = { zvs->GetBufferPointer(), zvs->GetBufferSize() }; zpd.PS = { zps->GetBufferPointer(), zps->GetBufferSize() };
            zpd.SampleMask = 0xFFFFFFFF;
            zpd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; zpd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; zpd.RasterizerState.DepthClipEnable = FALSE;
            zpd.DepthStencilState.DepthEnable = TRUE; zpd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
            zpd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS; zpd.DepthStencilState.StencilEnable = FALSE;
            zpd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            zpd.NumRenderTargets = 0; zpd.DSVFormat = kDepthFmt; zpd.SampleDesc.Count = 1;
            g_dev->CreateGraphicsPipelineState(&zpd, IID_PPV_ARGS(&g_colorToDepthPso));
            if (ScreenScaleX() > 1)
            {
                const std::string kSrc = "#define SWAPK " + std::to_string(ScreenScaleX()) + "\n" + c2dSrc;
                auto kps = Compile(kSrc, "ps_main", "ps_5_0", "color2depthk");
                if (kps) { zpd.PS = { kps->GetBufferPointer(), kps->GetBufferSize() }; g_dev->CreateGraphicsPipelineState(&zpd, IID_PPV_ARGS(&g_colorToDepthPsoK)); }
            }
        }
    }

    // Render target "EDRAM" (color 0) + textura blanca provisional + buffers de subida
    D3D12_HEAP_PROPERTIES def{ D3D12_HEAP_TYPE_DEFAULT }, up{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = kRtW; td.Height = kRtH; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv{ DXGI_FORMAT_B8G8R8A8_UNORM, { 0, 0, 0, 1 } };
    g_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&g_edram));
    g_dev->CreateRenderTargetView(g_edram.Get(), nullptr, Rtv(kFrames));
    g_dev->CreateShaderResourceView(g_edram.Get(), nullptr, SrvCpu(17));
    td.Width = 1; td.Height = 1; td.Flags = D3D12_RESOURCE_FLAG_NONE;
    g_dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_white));
    // (Textura 1x1 en heap de subida no es valida para SRV en todos los drivers: usamos una por defecto rellenada por copia)
    g_white.Reset();
    g_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_white));
    for (UINT i = 1; i <= 16; i++) g_dev->CreateShaderResourceView(g_white.Get(), nullptr, SrvCpu(i));
    D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = kRingSize; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (UINT i = 0; i < kFrames; i++)
    {
        g_dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_ring[i]));
        D3D12_RANGE none{ 0, 0 }; g_ring[i]->Map(0, &none, (void**)&g_ringMap[i]);
    }
    LOG("[d3d12] backend inicializado (swapchain %ux%u, EDRAM provisional, draws traducidos)", kRtW, kRtH);
    PrewarmShaders();
    return true;
}

// Sesion 24: la cadena de intercambio sigue el tamano de la ventana (antes 1280x720 fijo y Windows la estiraba con un
// filtro bilineal: todo borroso en ventanas grandes). Se llama tras Present, sin frame abierto.
void ResizeSwapIfNeeded()
{
    static const bool fixedSize = getenv("GOWJ_FIXEDSWAP") != nullptr;
    if (fixedSize || !g_presentHwnd || !g_swap || g_frameOpen) return;
    RECT rc; if (!GetClientRect(g_presentHwnd, &rc)) return;
    UINT w = (UINT)(rc.right - rc.left), h = (UINT)(rc.bottom - rc.top);
    if (w < 64 || h < 64 || (w == g_bbW && h == g_bbH)) return;   // minimizada o sin cambios
    g_queue->Signal(g_fence.Get(), g_nextFence);
    g_fence->SetEventOnCompletion(g_nextFence++, g_fenceEvent);
    WaitForSingleObject(g_fenceEvent, 5000);
    for (UINT i = 0; i < kFrames; i++) g_backbuffers[i].Reset();
    HRESULT hr = g_swap->ResizeBuffers(kFrames, w, h, DXGI_FORMAT_UNKNOWN, g_swapFlags);
    for (UINT i = 0; i < kFrames; i++)
    {
        g_swap->GetBuffer(i, IID_PPV_ARGS(&g_backbuffers[i]));
        if (g_backbuffers[i]) g_dev->CreateRenderTargetView(g_backbuffers[i].Get(), nullptr, Rtv(i));
    }
    if (SUCCEEDED(hr)) { g_bbW = w; g_bbH = h; }
    LOG("[d3d12] ventana %ux%u: cadena de intercambio redimensionada (0x%08lX)", w, h, (unsigned long)hr);
}
// Rectangulo 16:9 centrado en la cadena de intercambio
// Sesion 25: panoramico (GOWJ_PANO / panoramico= en gowj.ini): 16:9 | auto (proporcion del monitor) | 21:9 | 32:9 | <numero>.
// El juego elige la proporcion de su camara entre dos constantes (16/9 en 0x8228925C si la pantalla es panoramica, 4/3 en 0x822817DC);
// se cambia la de 16/9 por la elegida y la imagen (720p) se presenta con esa proporcion.
} // namespace
float PanoAspect()
{
    static float a = [] {
        const char* s = getenv("GOWJ_PANO"); float r = 16.0f / 9.0f;
        if (!s || !*s || !strcmp(s, "16:9")) return r;
        if (!strcmp(s, "21:9")) r = 64.0f / 27.0f;
        else if (!strcmp(s, "32:9")) r = 32.0f / 9.0f;
        else if (!strcmp(s, "auto")) { MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
                                       float m = (float)(mi.rcMonitor.right - mi.rcMonitor.left) / std::max(1L, mi.rcMonitor.bottom - mi.rcMonitor.top); if (m > r) r = m; }
        else { float v = (float)atof(s); if (v > 1.0f && v < 6.0f) r = v; }
        return std::max(r, 16.0f / 9.0f);
    }();
    return a;
}
uint32_t ResK()
{
    static const uint32_t k = [] { const char* e = getenv("GOWJ_RESINTERNA"); int v = e ? atoi(e) : 1; return (uint32_t)std::clamp(v, 1, 3); }();
    return k;
}
uint32_t ScreenScaleX() { return PanoWide() * ResK(); }   // escala X total de las superficies de pantalla (gpu_shader.cpp)
uint32_t PanoWide()
{
    static const uint32_t s = [] {
        float kx = PanoAspect() / (16.0f / 9.0f);
        if (kx <= 1.001f || getenv("GOWJ_PANOSTRETCH")) return 1u;
        return (uint32_t)std::min(4.0f, std::ceil(kx - 0.01f));
    }();
    return s;
}
void ApplyPanoCamera()
{
    float a = PanoAspect(); if (a <= 16.0f / 9.0f + 1e-3f) return;
    uint32_t u; memcpy(&u, &a, 4); *(uint32_t*)(g_base + 0x8228925C) = __builtin_bswap32(u);
    LOG("[d3d12] panoramico: proporcion de la camara %.3f (constante 16/9 del juego sustituida)", a);
}
namespace {
void PresentViewport(D3D12_VIEWPORT& vp, D3D12_RECT& sr)
{
    float pa = PanoActive() ? PanoAspect() : 16.0f / 9.0f;
    float sc = std::min(g_bbW / ((float)kRtH * pa), g_bbH / (float)kRtH);
    float w = kRtH * pa * sc, h = kRtH * sc;
    vp = { std::floor((g_bbW - w) * 0.5f), std::floor((g_bbH - h) * 0.5f), w, h, 0, 1 };
    sr = { 0, 0, (LONG)g_bbW, (LONG)g_bbH };
}
// GoWJ sesion 11: pinta los laterales (fuera del rectangulo 16:9) con la imagen reflejada y desenfocada (kSideHlsl)
void PresentSides(D3D12_GPU_DESCRIPTOR_HANDLE srv)
{
    if (!g_sidePso || PanoActive()) return;
    D3D12_VIEWPORT vp; D3D12_RECT sr; PresentViewport(vp, sr);
    float side = vp.TopLeftX; if (side < 4.0f) return;
    D3D12_VIEWPORT full{ 0, 0, (float)g_bbW, (float)g_bbH, 0, 1 };
    g_list->RSSetViewports(1, &full); g_list->RSSetScissorRects(1, &sr);
    g_list->SetPipelineState(g_sidePso.Get());
    g_list->SetGraphicsRootSignature(g_sideRootSig.Get());
    ID3D12DescriptorHeap* heaps[] = { g_srvHeap.Get() };
    g_list->SetDescriptorHeaps(1, heaps);
    g_list->SetGraphicsRootDescriptorTable(0, srv);
    float c[4] = { vp.TopLeftX, vp.TopLeftX + vp.Width, side, (float)g_bbH };
    g_list->SetGraphicsRoot32BitConstants(1, 4, c, 0);
    g_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_list->DrawInstanced(3, 1, 0, 0);
}

UINT64 RingAlloc(UINT64 size, UINT64 align = 256)
{
    UINT64 o = (g_ringOffset + align - 1) & ~(align - 1);
    if (o + size > kRingSize)
    {
        // Sesion 24: el anillo por frame se llena (los draws siguientes se omiten como "mem"): se registra
        static int logged = 0; static UINT64 lastFrame = ~0ull;
        if (logged < 40 && lastFrame != g_frameCounter) { logged++; lastFrame = g_frameCounter; LOG("[d3d12] anillo lleno en el frame %llu: usado %llu MB, pedido %llu KB", (unsigned long long)g_frameCounter, (unsigned long long)(g_ringOffset >> 20), (unsigned long long)(size >> 10)); }
        return ~0ull;
    }
    g_ringOffset = o + size;
    return o;
}

// ---- Sesion 25: contador en pantalla (F3; mostrar_fps=1 en gowj.ini / GOWJ_FPS=1). Basado en el de Guitar Hero, con mas datos:
// FPS, tiempo por frame (media, maximo y 1% mas lento), tirones, draws, CPU del proceso, memoria de video y RAM, resolucion y audio.
// Se dibuja con GDI en una textura (alfa premultiplicado) y se pinta encima de la imagen en la esquina superior izquierda.
ComPtr<ID3D12PipelineState> g_overlayPso; ComPtr<ID3D12Resource> g_overlayTex;
D3D12_RESOURCE_STATES g_overlayState = D3D12_RESOURCE_STATE_COPY_DEST;
constexpr UINT kOverlaySrvSlot = 22, kOverlayW = 512, kOverlayH = 128;   // 512*4 = 2048 bytes por fila (multiplo de 256)
std::vector<uint32_t> g_overlayPixels; bool g_overlayDirty = false;
} // namespace
std::atomic<bool> g_showStats{ getenv("GOWJ_FPS") && atoi(getenv("GOWJ_FPS")) != 0 };   // F3 (gpu_window.cpp)
size_t AudioQueuedFrames();   // audio_wasapi.cpp
namespace {
static void OverlayRender(const std::vector<std::pair<std::string, COLORREF>>& lines)
{
    static HDC dc = nullptr; static HBITMAP bmp = nullptr; static uint32_t* bits = nullptr; static HFONT font = nullptr;
    if (!dc)
    {
        dc = CreateCompatibleDC(nullptr);
        BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = kOverlayW; bi.bmiHeader.biHeight = -(int)kOverlayH;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&bits, nullptr, 0);
        SelectObject(dc, bmp);
        font = CreateFontW(-17, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, FIXED_PITCH, L"Consolas");
        SelectObject(dc, font);
    }
    if (!bits) return;
    g_overlayPixels.assign(kOverlayW * kOverlayH, 0);
    const int lineH = 22, pad = 8;
    int panelW = 0, panelH = pad * 2 + lineH * (int)lines.size();
    for (size_t li = 0; li < lines.size(); li++)
    {
        memset(bits, 0, kOverlayW * kOverlayH * 4);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
        wchar_t wt[160]; int wn = MultiByteToWideChar(CP_UTF8, 0, lines[li].first.c_str(), -1, wt, 160); if (wn <= 0) { wt[0] = 0; wn = 1; }
        SIZE sz{}; GetTextExtentPoint32W(dc, wt, wn - 1, &sz); panelW = std::max<int>(panelW, sz.cx + pad * 2);
        int y0 = pad + (int)li * lineH;
        TextOutW(dc, pad, y0 + (lineH - sz.cy) / 2, wt, wn - 1);
        COLORREF c = lines[li].second; float cr = GetRValue(c) / 255.0f, cg = GetGValue(c) / 255.0f, cb = GetBValue(c) / 255.0f;
        for (int y = y0; y < y0 + lineH && y < (int)kOverlayH; y++)
            for (UINT x = 0; x < kOverlayW; x++)
            {
                uint32_t q = bits[y * kOverlayW + x];
                float t = std::max({ (q >> 16) & 255, (q >> 8) & 255, q & 255 }) / 255.0f;
                if (t <= 0) continue;
                uint32_t r = (uint32_t)(cr * t * 255), gg = (uint32_t)(cg * t * 255), b = (uint32_t)(cb * t * 255), al = (uint32_t)(t * 255);
                g_overlayPixels[y * kOverlayW + x] = b | (gg << 8) | (r << 16) | (al << 24);
            }
    }
    panelW = std::min<int>(panelW, kOverlayW); panelH = std::min<int>(panelH, kOverlayH);
    for (int y = 0; y < panelH; y++)   // fondo oscuro translucido detras del texto
        for (int x = 0; x < panelW; x++)
        {
            uint32_t& q = g_overlayPixels[y * kOverlayW + x];
            float ta = (q >> 24) / 255.0f, a = ta + 0.62f * (1 - ta);
            q = (q & 0x00FFFFFF) | ((uint32_t)(a * 255) << 24);
        }
    g_overlayDirty = true;
}
// Estadisticas por presentacion; el texto se rehace cada 0,5 s
void OverlayTick(uint64_t drawsThisFrame)
{
    static double lastT = 0, winStart = 0; static std::vector<double> dts; static uint64_t draws = 0; static int hitchesTotal = 0;
    static FILETIME lastK{}, lastU{}; static double lastCpuT = 0; static double cpuPct = 0;
    double now = QpcMs();
    // tirones: solo en partida (las pantallas de carga van a 20 fps a proposito y contaban como tirones)
    if (lastT > 0) { double dt = now - lastT; dts.push_back(dt); if (dt > 40 && g_framesNoScene == 0) hitchesTotal++; }
    lastT = now; draws += drawsThisFrame;
    if (!winStart) winStart = now;
    if (!g_showStats || now - winStart < 500 || dts.empty()) { if (now - winStart >= 500) { winStart = now; dts.clear(); draws = 0; } return; }
    double fps = dts.size() * 1000.0 / (now - winStart), sum = 0, mx = 0;
    for (double d : dts) { sum += d; mx = std::max(mx, d); }
    std::vector<double> srt = dts; std::sort(srt.begin(), srt.end());
    double p99 = srt[std::min(srt.size() - 1, (size_t)(srt.size() * 0.99))];
    {   // CPU del proceso (todos los nucleos = 100%)
        FILETIME c, e, k, u; GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
        auto v = [](FILETIME f) { return (double)((uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 10000.0; };
        if (lastCpuT > 0) { static const unsigned nc = std::max(1u, std::thread::hardware_concurrency()); cpuPct = ((v(k) - v(lastK)) + (v(u) - v(lastU))) / (now - lastCpuT) / nc * 100.0; }
        lastK = k; lastU = u; lastCpuT = now;
    }
    double vramMB = -1, vramBudget = 0;
    {
        static ComPtr<IDXGIAdapter3> ad; static bool tried = false;
        if (!tried) { tried = true; ComPtr<IDXGIFactory4> f; if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) { ComPtr<IDXGIAdapter1> a1; LUID l = g_dev->GetAdapterLuid(); if (SUCCEEDED(f->EnumAdapterByLuid(l, IID_PPV_ARGS(&a1)))) a1.As(&ad); } }
        DXGI_QUERY_VIDEO_MEMORY_INFO mi{};
        if (ad && SUCCEEDED(ad->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mi))) { vramMB = mi.CurrentUsage / 1048576.0; vramBudget = mi.Budget / 1048576.0; }
    }
    PROCESS_MEMORY_COUNTERS pmc{}; GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    double audMs = AudioQueuedFrames() / 48.0;
    char l1[128], l2[128], l3[128], l4[128];
    snprintf(l1, sizeof(l1), "%3.0f FPS   %4.1f ms  (max %3.0f, 1%% %4.1f)", fps, sum / dts.size(), mx, p99);
    snprintf(l2, sizeof(l2), "CPU %3.0f%%   draws %5.0f/frame   tirones %d", cpuPct, (double)draws / dts.size(), hitchesTotal);
    if (vramMB >= 0) snprintf(l3, sizeof(l3), "VRAM %5.0f / %5.0f MB   RAM %5.0f MB", vramMB, vramBudget, pmc.WorkingSetSize / 1048576.0);
    else snprintf(l3, sizeof(l3), "RAM %5.0f MB", pmc.WorkingSetSize / 1048576.0);
    snprintf(l4, sizeof(l4), "%ux%u -> %ux%u   audio %3.0f ms", kRtW * (PanoActive() ? ScreenScaleX() : ResK()), kRtH * ResK(), g_bbW, g_bbH, audMs);   // GoWJ sesion 11: resolucion interna real
    COLORREF fc = fps >= 57 ? RGB(110, 255, 120) : fps >= 45 ? RGB(255, 220, 80) : RGB(255, 90, 80);
    COLORREF wc = RGB(225, 225, 225);
    OverlayRender({ { l1, fc }, { l2, wc }, { l3, wc }, { l4, wc } });
    winStart = now; dts.clear(); draws = 0;
}
void OverlayDraw(UINT fi)
{
    if (!g_showStats || !g_overlayPso || g_overlayPixels.empty()) return;
    if (!g_overlayTex)
    {
        D3D12_HEAP_PROPERTIES def{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = kOverlayW; td.Height = kOverlayH; td.DepthOrArraySize = 1;
        td.MipLevels = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        if (FAILED(g_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_overlayTex)))) return;
        g_overlayState = D3D12_RESOURCE_STATE_COPY_DEST;
        g_dev->CreateShaderResourceView(g_overlayTex.Get(), nullptr, SrvCpu(kOverlaySrvSlot));
    }
    if (g_overlayDirty)
    {
        const UINT pitch = kOverlayW * 4;
        UINT64 o = RingAlloc(UINT64(pitch) * kOverlayH, 512);
        if (o != ~0ull)
        {
            memcpy(g_ringMap[fi] + o, g_overlayPixels.data(), pitch * kOverlayH);
            if (g_overlayState != D3D12_RESOURCE_STATE_COPY_DEST) { auto b = Transition(g_overlayTex.Get(), g_overlayState, D3D12_RESOURCE_STATE_COPY_DEST); g_list->ResourceBarrier(1, &b); }
            D3D12_TEXTURE_COPY_LOCATION dst{ g_overlayTex.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; dst.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION src{ g_ring[fi].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
            src.PlacedFootprint.Offset = o; src.PlacedFootprint.Footprint = { DXGI_FORMAT_B8G8R8A8_UNORM, kOverlayW, kOverlayH, 1, pitch };
            g_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            auto b = Transition(g_overlayTex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); g_list->ResourceBarrier(1, &b);
            g_overlayState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            g_overlayDirty = false;
        }
    }
    if (g_overlayState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) return;
    D3D12_VIEWPORT vp; D3D12_RECT sr; PresentViewport(vp, sr);
    float sc = std::max(0.75f, vp.Height / 720.0f);
    D3D12_VIEWPORT ov{ vp.TopLeftX + 12 * sc, vp.TopLeftY + 12 * sc, kOverlayW * sc, kOverlayH * sc, 0, 1 };
    D3D12_RECT os{ (LONG)ov.TopLeftX, (LONG)ov.TopLeftY, (LONG)(ov.TopLeftX + ov.Width), (LONG)(ov.TopLeftY + ov.Height) };
    g_list->RSSetViewports(1, &ov); g_list->RSSetScissorRects(1, &os);
    g_list->SetPipelineState(g_overlayPso.Get());
    g_list->SetGraphicsRootSignature(g_blitRootSig.Get());
    ID3D12DescriptorHeap* heaps[] = { g_srvHeap.Get() };
    g_list->SetDescriptorHeaps(1, heaps);
    g_list->SetGraphicsRootDescriptorTable(0, SrvGpu(kOverlaySrvSlot));
    g_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_list->DrawInstanced(3, 1, 0, 0);
}

void OpenFrame()
{
    if (g_frameOpen) return;
    if (g_frameWait) { double tw = QpcMs(); WaitForSingleObjectEx(g_frameWait, 100, TRUE); g_hWaitMs += QpcMs() - tw; }
    g_fi = g_swap->GetCurrentBackBufferIndex();
    if (g_fence->GetCompletedValue() < g_fenceValues[g_fi]) { double tw = QpcMs(); g_fence->SetEventOnCompletion(g_fenceValues[g_fi], g_fenceEvent); WaitForSingleObject(g_fenceEvent, 1000); g_hWaitMs += QpcMs() - tw; }
    g_alloc[g_fi]->Reset();
    g_list->Reset(g_alloc[g_fi].Get(), nullptr);
    g_ringOffset = 0;
    g_vbCache.clear();
    g_deferredRelease[g_fi].clear();   // la GPU ya termino el frame que uso este indice
    static bool whiteDone = false;
    if (!whiteDone)
    {
        whiteDone = true;
        UINT64 o = RingAlloc(256, 512);
        *(uint32_t*)(g_ringMap[g_fi] + o) = 0xFFFFFFFF;
        D3D12_TEXTURE_COPY_LOCATION dst{ g_white.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src{ g_ring[g_fi].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        src.PlacedFootprint.Offset = o; src.PlacedFootprint.Footprint = { DXGI_FORMAT_B8G8R8A8_UNORM, 1, 1, 1, 256 };
        g_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        auto b = Transition(g_white.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        ProbedBarrier(1, &b, __LINE__);
    }
    g_boundRt = nullptr;
    g_frameOpen = true;
    g_frameDraws = 0;
    g_descCursor = 0;
    g_frameCounter++;
}

// ---- traduccion de estado ----
inline uint32_t Reg(uint32_t i) { return Load32(0x7FC80000 + i * 4); }
inline float RegF(uint32_t i) { uint32_t v = Reg(i); float f; memcpy(&f, &v, 4); return f; }
// PA_SC_WINDOW_OFFSET (0x2080), aplicado si PA_SU_SC_MODE_CNTL.vtx_window_offset_enable (bit 16): enteros con signo de 15 bits (sesion 23)
inline void WindowOffset(int& x, int& y)
{
    x = y = 0;
    if (!(Reg(0x2205) & (1u << 16))) return;
    uint32_t v = Reg(0x2080);
    x = (int32_t)(v << 17) >> 17; y = (int32_t)((v >> 16) << 17) >> 17;
}

D3D12_BLEND MapBlend(uint32_t b)
{
    switch (b)
    {
    case 0: return D3D12_BLEND_ZERO; case 1: return D3D12_BLEND_ONE;
    case 4: return D3D12_BLEND_SRC_COLOR; case 5: return D3D12_BLEND_INV_SRC_COLOR;
    case 6: return D3D12_BLEND_SRC_ALPHA; case 7: return D3D12_BLEND_INV_SRC_ALPHA;
    case 8: return D3D12_BLEND_DEST_COLOR; case 9: return D3D12_BLEND_INV_DEST_COLOR;
    case 10: return D3D12_BLEND_DEST_ALPHA; case 11: return D3D12_BLEND_INV_DEST_ALPHA;
    case 12: case 14: return D3D12_BLEND_BLEND_FACTOR; case 13: case 15: return D3D12_BLEND_INV_BLEND_FACTOR;
    case 16: return D3D12_BLEND_SRC_ALPHA_SAT;
    default: return D3D12_BLEND_ONE;
    }
}
D3D12_BLEND MapBlendAlpha(uint32_t b)
{
    D3D12_BLEND x = MapBlend(b);
    if (x == D3D12_BLEND_SRC_COLOR) return D3D12_BLEND_SRC_ALPHA;
    if (x == D3D12_BLEND_INV_SRC_COLOR) return D3D12_BLEND_INV_SRC_ALPHA;
    if (x == D3D12_BLEND_DEST_COLOR) return D3D12_BLEND_DEST_ALPHA;
    if (x == D3D12_BLEND_INV_DEST_COLOR) return D3D12_BLEND_INV_DEST_ALPHA;
    return x;
}
D3D12_BLEND_OP MapOp(uint32_t o)
{
    switch (o) { case 1: return D3D12_BLEND_OP_SUBTRACT; case 2: return D3D12_BLEND_OP_MIN; case 3: return D3D12_BLEND_OP_MAX; case 4: return D3D12_BLEND_OP_REV_SUBTRACT; default: return D3D12_BLEND_OP_ADD; }
}

// Compila (o recupera de la cache) el shader de una entrada ya traducida; deja e.blob/e.ok y marca state = 2.
static void CompileShaderEntry(ShaderEntry& e, uint32_t type, const uint32_t* code, uint32_t count, uint64_t hash)
{
    char name[64]; snprintf(name, sizeof(name), "%s_%016llX", type ? "ps" : "vs", (unsigned long long)hash);
    ComPtr<ID3DBlob> blob = Compile(e.info.hlsl, "main", type ? "ps_5_0" : "vs_5_0", name);
    if (!blob && type == 1 && e.info.hlsl.find("SampleGrad") != std::string::npos && code)
    {
        // Sesion 24: los gradientes dentro de flujo de control variable no compilan: se retraduce con nivel 0 fijo
        TranslatedShader t2;
        { std::lock_guard<std::mutex> lk(g_xlatMutex); g_xlatNoGrad = true; t2 = TranslateXenosShader(type, code, count); g_xlatNoGrad = false; }
        if (t2.ok) ApplyShaderOverrides(type, hash, t2.hlsl);
        if (t2.ok) { e.info = t2; blob = Compile(e.info.hlsl, "main", "ps_5_0", name); LOG("[d3d12] PS %016llX sin gradientes (nivel 0): %s", (unsigned long long)hash, blob ? "ok" : "falla"); }
    }
    e.blob = blob; e.ok = blob != nullptr;
    e.state.store(2, std::memory_order_release);
}
// Sesion 23: compilacion asincrona. Un shader nuevo se traduce en el momento y se compila en un grupo de hilos propio; mientras,
// sus draws se omiten (antes el primer arranque se congelaba 31 s en total: 54 shaders seguidos en un corte = 12,6 s).
// GOWJ_SYNCSHADERS=1 = compilar en el momento como antes.
struct CompileJob { ShaderEntry* e; uint32_t type; std::vector<uint32_t> code; uint64_t hash; };
static void EnqueueCompile(CompileJob job)
{
    static std::mutex m; static std::condition_variable cv; static std::vector<CompileJob> q;
    static std::once_flag once;
    std::call_once(once, [] {
        unsigned n = std::max(2u, std::min(6u, std::thread::hardware_concurrency() / 2));
        for (unsigned i = 0; i < n; i++)
            std::thread([] {
                t_prewarm = true;   // no cuenta en las estadisticas de tirones del hilo de la GPU
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                for (;;)
                {
                    CompileJob j;
                    { std::unique_lock<std::mutex> l(m); cv.wait(l, [] { return !q.empty(); }); j = std::move(q.back()); q.pop_back(); }
                    CompileShaderEntry(*j.e, j.type, j.code.data(), (uint32_t)j.code.size(), j.hash);
                }
            }).detach();
    });
    { std::lock_guard<std::mutex> l(m); q.push_back(std::move(job)); }
    cv.notify_one();
}
ShaderEntry& GetShader(uint32_t type, const uint32_t* code, uint32_t count, uint64_t hash)
{
    auto it = g_shaders.find(hash);
    if (it != g_shaders.end())
    {
        ShaderEntry& e = it->second;
        if (e.state.load(std::memory_order_acquire) == 1) { static ShaderEntry pending; return pending; }   // aun compilando: ok = false
        return e;
    }
    ShaderEntry& e = g_shaders[hash];
    { std::lock_guard<std::mutex> lk(g_xlatMutex); e.info = TranslateXenosShader(type, code, count); }
    if (e.info.ok && ApplyShaderOverrides(type, hash, e.info.hlsl)) LOG("[d3d12] sustitucion aplicada a %016llX", (unsigned long long)hash);
    if (!e.info.ok) { LOG("[d3d12] shader %016llX no traducido: %s", (unsigned long long)hash, e.info.error.c_str()); e.state = 2; return e; }
    static const bool sync = getenv("GOWJ_SYNCSHADERS") != nullptr;
    // GoWJ sesion 11: los VS con memexport escriben en la memoria del juego (UE3 mueve texturas al desfragmentar con un VS de copia).
    // Si el draw se omitia mientras el shader se compilaba, la copia se perdia para siempre: la textura movida (fuente, HUD, armas,
    // armaduras) quedaba con basura toda la partida. Se compilan al momento. GOWJ_ASYNCMEMEX=1 = antes.
    static const bool asyncMemex = getenv("GOWJ_ASYNCMEMEX") != nullptr;
    if (sync || (type == 0 && e.info.memexport && !asyncMemex)) { CompileShaderEntry(e, type, code, count, hash); return e; }
    {   // ya en la cache de disco: al momento
        char name[64]; snprintf(name, sizeof(name), "%s_%016llX", type ? "ps" : "vs", (unsigned long long)hash);
        t_cacheOnly = true; ComPtr<ID3DBlob> blob = Compile(e.info.hlsl, "main", type ? "ps_5_0" : "vs_5_0", name); t_cacheOnly = false;
        if (blob) { e.blob = blob; e.ok = true; e.state = 2; return e; }
    }
    e.state = 1;
    EnqueueCompile({ &e, type, std::vector<uint32_t>(code, code + count), hash });
    static ShaderEntry pending; return pending;
}

uint64_t g_skipped[16] = {};

// ---- texturas (sesion 8): solo mip base, formatos 8888 y DXT1/3/5 ----
// Sesion 23 (rendimiento): las texturas se crean como recursos colocados en montones de 64 MB (asignacion secuencial; la cache de
// texturas nunca libera). CreateCommittedResource costaba ~0,2 ms por textura: 535 texturas nuevas en un corte = 116 ms de tiron.
// Las de mas de 16 MB, o si algo falla, siguen siendo committed. GOWJ_TEXCOMMITTED=1 = como antes.
static HRESULT CreateTextureResource(const D3D12_RESOURCE_DESC& td, ComPtr<ID3D12Resource>& out)
{
    static const bool committed = getenv("GOWJ_TEXCOMMITTED") != nullptr;
    D3D12_HEAP_PROPERTIES def{ D3D12_HEAP_TYPE_DEFAULT };
    if (!committed)
    {
        static std::vector<ComPtr<ID3D12Heap>> heaps; static UINT64 used = 0; const UINT64 kHeap = 64ull << 20;
        D3D12_RESOURCE_ALLOCATION_INFO ai = g_dev->GetResourceAllocationInfo(0, 1, &td);
        if (ai.SizeInBytes != UINT64_MAX && ai.SizeInBytes <= (16ull << 20))
        {
            UINT64 al = ai.Alignment ? ai.Alignment : 65536, off = (used + al - 1) & ~(al - 1);
            if (heaps.empty() || off + ai.SizeInBytes > kHeap)
            {
                D3D12_HEAP_DESC hd{}; hd.SizeInBytes = kHeap; hd.Properties = def; hd.Alignment = D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT;
                hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
                ComPtr<ID3D12Heap> h;
                if (SUCCEEDED(g_dev->CreateHeap(&hd, IID_PPV_ARGS(&h)))) { heaps.push_back(h); used = 0; off = 0; }
                else return g_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&out));
            }
            HRESULT hr = g_dev->CreatePlacedResource(heaps.back().Get(), off, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&out));
            if (SUCCEEDED(hr)) { used = off + ai.SizeInBytes; return hr; }
        }
    }
    return g_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&out));
}
struct TexEntry { ComPtr<ID3D12Resource> res; DXGI_FORMAT fmt; uint64_t dataHash = 0; uint64_t lastFrame = 0; uint64_t resolveGen = 0; uint32_t levels = 1; };
// Sesion 24: generacion de resolves por CPU por direccion de destino. Una textura se vuelve a subir si hubo un resolve nuevo a su
// direccion desde su ultima subida, aunque el hash de muestras coincida (texturas casi uniformes como la SSAO).
std::unordered_map<uint32_t, uint64_t> g_cpuResolveGen;
uint64_t g_cpuResolveCounter = 0;
uint64_t g_writeBacks = 0, g_gpuResolvesWin = 0, g_cpuResolvesWin = 0;   // volcados de texturas resueltas por GPU a memoria (estadistica, sesion 24)
void FlushPendingResolves();
void FlushResolvesIfOverlap(uint32_t lo, uint32_t hi);
bool g_pendingResolvesEmpty();
// Resolves por GPU (sesion 21): el resultado se queda en una textura de la GPU asociada a la direccion de destino, sin pasar por la CPU.
struct ResolvedTex
{
    ComPtr<ID3D12Resource> res; UINT w = 0, h = 0; bool swapRB = false; uint32_t endian = 0; uint64_t frame = 0;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
    uint32_t destBase = 0, destPitch = 0, destH = 0;   // destino en memoria (para volcarla si se lee con otro formato/tamano)
    bool inMemory = false;                             // ya volcada a la memoria del juego en este estado
    uint64_t memHash = 0;                              // huella de la memoria de destino al resolver: si cambia, la CPU la ha reescrito
    uint64_t gen = 0;                                  // numero de resolve (para rehacer copias derivadas dentro del mismo frame)
    DXGI_FORMAT fmt = DXGI_FORMAT_B8G8R8A8_UNORM;      // sesion 24: formato del recurso (B8G8R8A8 o R32_FLOAT)
    uint32_t texFmt = 6;                               // formato de textura de Xenos del destino (6 = 8888, 2 = k_8, 36 = k_32_FLOAT)
    UINT rtv = ~0u;                                    // RTV en g_rtvHeap3 (resolves por shader)
    UINT sx = 1, sy = 1;                               // sesion 27: el recurso mide w * sx de ancho (superficie panoramica); sy: alto (resolucion interna)
    bool screen = false;                               // sesion 27: resuelta desde una superficie de pantalla (la escena)
    uint64_t wsig = 0; uint32_t wlen = 0;              // GoWJ sesion 11: firma por pagina del destino (escrituras de la CPU tras el resolve)
};
// GoWJ sesion 11: la copia de un resolve se queda en la GPU; la memoria de destino no se escribe. Si despues el juego reutiliza esa
// memoria (carga ahi una textura: letras, iconos del HUD, mascaras de las armas y armaduras), la entrada vieja seguia valiendo: se
// enlazaba la imagen de la pantalla en lugar de la textura, o WriteBackResolvedForDraw la volcaba encima de la textura recien cargada
// (texto/HUD rotos, arma cian, personajes con tramas, fijos toda la partida e intermitentes entre partidas). La huella MemHash (64
// muestras en 256 KB) no veia texturas pequenas. Ahora se vigilan las paginas del destino. GOWJ_NORESOLVESIG=1 = antes.
static void ResolveSigTake(ResolvedTex& r)
{
    static const bool off = getenv("GOWJ_NORESOLVESIG") != nullptr;
    if (off) { r.wlen = 0; return; }
    uint32_t bpp = (r.texFmt == 32 || r.texFmt == 26) ? 8 : r.texFmt == 2 ? 1 : r.texFmt == 4 ? 2 : 4;
    uint64_t size = uint64_t((r.destPitch + 31) & ~31u) * (((r.destH ? r.destH : r.h) + 31) & ~31u) * bpp;
    size &= ~0xFFFull; if (size < 0x1000) size = 0x1000; if (size > (4u << 20)) size = 4u << 20;
    r.wlen = (uint32_t)size; r.wsig = TexDirtyPageSig(r.destBase, r.wlen);
}
static bool ResolveCpuOverwritten(const ResolvedTex& r)
{
    return r.wlen && !TexDirtyPageValid(r.destBase, r.wlen, r.wsig);
}
inline uint64_t MemHash(uint32_t addr);
// GoWJ sesion 11: la copia de la GPU sigue valiendo si la CPU no ha escrito en las paginas del destino. Antes se miraba MemHash, que
// muestrea 256 KB: la cache de glifos de Scaleform son texturas k_8 de 64x64 (4 KB) cada 64 KB y cualquier escritura en las vecinas
// cambiaba la huella -> se leia la memoria, donde el glifo nunca se escribio (letras sin sentido, mas con mucha carga y en 32:9).
static inline bool ResolveMemOk(const ResolvedTex& r, uint32_t base)
{
    if (r.wlen) return !ResolveCpuOverwritten(r);
    return r.inMemory || MemHash(base) == r.memHash;
}
// Olvida el ultimo resolve de esta direccion si la CPU ha escrito encima despues
extern std::unordered_map<uint64_t, ResolvedTex> g_resolved;
extern std::unordered_map<uint32_t, uint64_t> g_resolvedLast;
static void PruneStaleResolve(uint32_t base)
{
    auto last = g_resolvedLast.find(base);
    if (last == g_resolvedLast.end()) return;
    auto it = g_resolved.find(last->second);
    if (it == g_resolved.end() || !ResolveCpuOverwritten(it->second)) return;
    static int n = 0;
    if (n++ < 24) LOG("[resolve] %08X (%ux%u fmt %u, frame %llu) reescrita por la CPU: se olvida la copia de la GPU", base, it->second.w, it->second.h, it->second.texFmt, (unsigned long long)it->second.frame);
    it->second.wlen = 0;
    g_resolvedLast.erase(last);
}
// Clave de una textura resuelta por GPU con formato distinto de 8888: los bits 32-43 de ResolvedKey estan libres
// GoWJ sesion 10 (del port de Gears 3, sesion 3): resolucion dinamica de UE3. Con la GPU cargada el juego dibuja la escena mas
// pequena (p. ej. 1280x433), la resuelve y la lee como textura de 1280x720 con las coordenadas escaladas (la region va en la
// esquina). Antes no "cabia", se volcaba a memoria y se leian restos de cuadros viejos (solo en las partidas en que se activaba).
// Se deja enlazar la copia de la GPU aunque se lea mas grande (hasta 1280x736 y 4 veces). GOWJ_NODYNRES=1 = antes.
static inline bool DynResFits(uint32_t w, uint32_t h, uint32_t rw, uint32_t rh)
{
    static const bool off = getenv("GOWJ_NODYNRES") != nullptr;
    return (w <= rw + 8 && h <= rh + 8) || (!off && w <= 1280 && h <= 736 && w >= rw && h >= rh && rw * 4 >= w && rh * 4 >= h);
}
inline uint64_t ResolvedFmtTag(uint32_t texFmt) { return texFmt == 6 ? 0 : (uint64_t(texFmt & 0x3F) << 32); }
std::unordered_map<uint64_t, ResolvedTex> g_resolved;      // clave: (direccion, ancho, alto)
std::unordered_map<uint32_t, uint64_t> g_resolvedLast;     // direccion -> clave del ultimo resolve por GPU
// Huella rapida de 64 dwords repartidos en los primeros 256 KB de una direccion fisica
inline uint64_t MemHash(uint32_t addr)
{
    const uint32_t* p = (const uint32_t*)(g_base + (0xA0000000u | (addr & 0x1FFFFFFF)));
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < 64; i++) { h ^= p[(i * 1031) % 65536]; h *= 1099511628211ull; }
    return h;
}
// GoWJ sesion 10 (rendimiento): huella de un bloque entero con 4 carriles de 64 bits independientes (el FNV de un dword cada vez es
// una cadena de multiplicaciones en serie: ~4 veces mas lento). Se usa en las texturas que se recorren enteras (k_8, <= 64 KB).
inline uint64_t HashDwordsFast(const uint32_t* p, uint32_t n, uint64_t h)
{
    const uint64_t P = 1099511628211ull;
    uint64_t a = h, b = h ^ 0x9E3779B97F4A7C15ull, c = h ^ 0xC2B2AE3D27D4EB4Full, d = h ^ 0x165667B19E3779F9ull;
    const uint8_t* q = (const uint8_t*)p; uint32_t blocks = n / 8;
    for (uint32_t i = 0; i < blocks; i++, q += 32)
    {
        uint64_t x[4]; memcpy(x, q, 32);
        a = (a ^ x[0]) * P; b = (b ^ x[1]) * P; c = (c ^ x[2]) * P; d = (d ^ x[3]) * P;
    }
    for (uint32_t i = blocks * 8; i < n; i++) a = (a ^ p[i]) * P;
    return (((a * P) ^ b) * P ^ c) * P ^ d;
}
inline uint64_t ResolvedKey(uint32_t addr, uint32_t w, uint32_t h) { return (uint64_t(addr & 0xFFFFF000) << 32) | (uint64_t(w & 0xFFFF) << 16) | (h & 0xFFFF); }
// GoWJ sesion 10: UE3 reutiliza la misma direccion (p. ej. 0B9F0000) para varios resolves por cuadro (escena 1280x720, bloom 640x360,
// 320x180, la caja de texto 609x65...). Solo se recordaba el ultimo: si el FXAA pedia la escena entera y el ultimo era otro, se leia la
// memoria con restos de cuadros viejos (niebla que sigue a la camara, arma azul, HUD y letras rotas). Si hay un resolve de este cuadro
// (o del anterior) con el tamano y formato exactos que se piden, se usa ese. GOWJ_NOEXACTRESOLVE=1 = antes.
std::unordered_map<uint64_t, ResolvedTex>::iterator ExactRecentResolve(uint32_t base, uint32_t w, uint32_t h, uint32_t texFmt);
uint64_t g_gpuResolves = 0;
std::unordered_map<uint64_t, ResolvedTex>::iterator ExactRecentResolve(uint32_t base, uint32_t w, uint32_t h, uint32_t texFmt)
{
    static const bool off = getenv("GOWJ_NOEXACTRESOLVE") != nullptr;
    if (off) return g_resolved.end();
    auto it = g_resolved.find(ResolvedKey(base, w, h) ^ ResolvedFmtTag(texFmt));
    if (it == g_resolved.end() || !it->second.res || it->second.texFmt != texFmt || g_frameCounter - it->second.frame > 1) return g_resolved.end();
    if (!ResolveMemOk(it->second, base)) return g_resolved.end();   // la CPU reescribio la memoria (sesion 11: por paginas)
    if (ResolveCpuOverwritten(it->second)) return g_resolved.end();   // GoWJ sesion 11
    return it;
}
// Sesion 24 (como Xenia, d3d12_texture_cache.cc): los formatos de 1 o 2 canales replican canales en el host
// (RRRR, RGGG, RGBB). Sin esto, leer .y/.w de una textura k_24_8 daba 0/1 en vez de la profundidad.
static UINT HostRep(uint32_t fmt, uint32_t c)
{
    static bool off = getenv("GOWJ_NOHOSTREP") != nullptr;
    if (off) return c;
    switch (fmt)
    {
    case 0: case 1: case 2: case 8: case 9: case 22: case 23: case 24: case 27: case 30: case 33: case 36: case 39: case 41: case 43: case 58: case 59:
        return 0;                                   // RRRR
    case 10: case 13: case 25: case 28: case 31: case 34: case 37: case 40: case 42: case 49:
        return c > 1 ? 1 : c;                       // RGGG
    case 16: case 17: case 55: case 57:
        return c > 2 ? 2 : c;                       // RGBB (GoWJ: 54 = 2_10_10_10_AS_16 tiene alfa)
    default: return c;
    }
}

// Resolves de profundidad por GPU (sesion 21): rz = R32_FLOAT (lectura como k_24_8), rp = RGBA8 con los bits 24_8 (lectura como 8888)
struct ResolvedDepth
{
    ComPtr<ID3D12Resource> rz, rp; UINT w = 0, h = 0, rtv = 0, sx = 1, sy = 1; bool screen = false; uint32_t endian = 0; uint64_t frame = 0;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    uint64_t gen = 0;   // sesion 8: numero de resolve (para rehacer las copias derivadas)
};
// GoWJ sesion 8: profundidad resuelta leida como k_24_8 MAYOR que el resolve (mapas de sombra: resolve 256x448 en el origen de una
// textura de 1120x688). Antes se leia la memoria del juego, que no tiene el resolve por GPU (datos viejos -> cunas negras de sombra).
struct DerivedDepth { ComPtr<ID3D12Resource> res; UINT w = 0, h = 0; uint64_t gen = 0; D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_COPY_DEST; };
std::unordered_map<uint64_t, DerivedDepth> g_derivedDepth;
std::unordered_map<uint64_t, ResolvedDepth> g_resolvedDepth;
std::unordered_map<uint32_t, uint64_t> g_resolvedDepthLast;
uint64_t g_texUploads = 0, g_texUploadBytes = 0, g_vbCopyBytes = 0;   // estadisticas por presentacion
std::unordered_map<uint64_t, TexEntry> g_textures;

uint32_t TiledOuter(uint32_t y, uint32_t pitch, uint32_t log2Bpp)
{
    uint32_t macro = ((y >> 5) * (pitch >> 5)) << (log2Bpp + 7);
    uint32_t micro = ((y & 6) << 2) << log2Bpp;
    return macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 8) << (3 + log2Bpp)) + ((y & 1) << 4);
}
uint32_t TiledInner(uint32_t x, uint32_t y, uint32_t log2Bpp, uint32_t base)
{
    uint32_t macro = (x >> 5) << (log2Bpp + 7);
    uint32_t micro = (x & 7) << log2Bpp;
    uint32_t off = base + (macro + ((micro & ~0xFu) << 1) + (micro & 0xF));
    return ((off & ~0x1FFu) << 3) + ((off & 0x1C0) << 2) + (off & 0x3F) + ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6);
}
inline uint32_t Swap32(uint32_t v, uint32_t e)
{
    switch (e) { case 1: return ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8); case 2: return __builtin_bswap32(v); case 3: return (v << 16) | (v >> 16); default: return v; }
}

// Crea (o reutiliza) la textura de la constante de fetch tf y escribe su SRV en 'slot'. false -> usar blanca.
uint32_t g_bindSy = 1;   // GoWJ sesion 11: escala Y (resolucion interna)
uint32_t g_bindSx = 1;   // sesion 27: escala X del recurso enlazado por el ultimo BindTexture (texturas resueltas panoramicas)
bool BindTexture(uint32_t tf, D3D12_CPU_DESCRIPTOR_HANDLE slot)
{
    g_bindSx = 1; g_bindSy = 1;
    uint32_t fc[6]; for (int i = 0; i < 6; i++) fc[i] = Reg(0x4800 + tf * 6 + i);
    if ((fc[0] & 3) != 2) return false;                    // no es constante de textura
    uint32_t fmt = fc[1] & 0x3F, endian = (fc[1] >> 6) & 3, base = fc[1] & 0xFFFFF000;
    uint32_t w = (fc[2] & 0x1FFF) + 1, h = ((fc[2] >> 13) & 0x1FFF) + 1;
    PruneStaleResolve(base);   // GoWJ sesion 11
    static const bool fontLog = getenv("GOWJ_FONTLOG") != nullptr;
    if (fontLog && fmt == 2 && w == 512 && h == 512)
    {   // diagnostico (GoWJ sesion 11, GOWJ_FONTLOG=1): direcciones de la fuente de campo de distancia (texto roto en 32:9)
        static std::unordered_map<uint32_t, int> seenF;
        static uint64_t lastF = 0;
        auto memFnv = [&]() { const uint32_t* q = (const uint32_t*)(g_base + 0xA0000000u + (base & 0x1FFFFFFF)); return HashDwordsFast(q, 0x10000, 1469598103934665603ull); };
        if (seenF.size() < 32 && seenF[base]++ == 0)
            LOG("[fuente] base %08X frame %llu fc %08X %08X %08X %08X %08X %08X PS %016llX resolve %d memoria %016llX", base, (unsigned long long)g_frameCounter, fc[0], fc[1], fc[2], fc[3], fc[4], fc[5], (unsigned long long)g_curPsHash, (int)(g_resolvedLast.count(base) != 0), (unsigned long long)memFnv());
        else if (g_frameCounter - lastF >= 300)
        {
            lastF = g_frameCounter;
            for (auto& kv : seenF)
            {
                const uint32_t* q = (const uint32_t*)(g_base + 0xA0000000u + (kv.first & 0x1FFFFFFF));
                LOG("[fuente] base %08X (en uso %08X) frame %llu memoria %016llX", kv.first, base, (unsigned long long)g_frameCounter, (unsigned long long)HashDwordsFast(q, 0x10000, 1469598103934665603ull));
            }
        }
    }
    if (!g_pendingResolvesEmpty())
    {   // resolves por CPU pendientes que escriben en esta textura (o sus mips): completarlos antes de leer la memoria
        uint32_t span = std::min<uint32_t>(std::max(w, 32u) * std::max(h, 32u) * 16u, 64u << 20);
        FlushResolvesIfOverlap(base, base + span);
        if (uint32_t ma = fc[5] & 0xFFFFF000) FlushResolvesIfOverlap(ma, ma + span);
    }
    bool tiled = fc[0] >> 31; uint32_t pitchTexels = ((fc[0] >> 22) & 0x1FF) * 32;
    uint32_t dim = (fc[5] >> 9) & 3;
    // Sesion 19 de NG2: textura 1D (ancho en los bits 0-23 de la palabra 2) = 2D de alto 1. Antes se rechazaba y el shader leia
    // la textura blanca de relleno (rampas de color/luz). GOWJ_NO1D=1 = antes.
    static const bool no1D = getenv("GOWJ_NO1D") != nullptr;
    if (dim == 0 && !no1D) { w = std::min<uint32_t>((fc[2] & 0xFFFFFF) + 1, 16384); h = 1; dim = 1; static int n = 0; if (n++ < 4) LOG("[d3d12] textura 1D %08X de %u texeles (fmt %u)", base, w, fmt); }
    if (dim != 1 || !base) return false;                   // solo 2D
    DXGI_FORMAT df; uint32_t blockBytes, blockDim;
    int conv = 0;   // 0 copia directa, 1 = profundidad 24_8 -> float, 2 = 24_8_FLOAT -> float, 3 = componentes de 16 bits con signo/exp -> half
    // Constante de fetch: dword0 bits 2-9 = signo por componente (1 = con signo), dword3 bits 13-18 = exp_adjust (con signo)
    uint32_t signBits = (fc[0] >> 2) & 0xFF;
    int expAdjust = (int)((fc[3] >> 13) & 0x3F); if (expAdjust & 0x20) expAdjust -= 64;
    bool signedOrExp = (signBits & 0x55) != 0 || expAdjust != 0;
    switch (fmt)
    {
    case 6: case 14: case 50: df = DXGI_FORMAT_R8G8B8A8_UNORM; blockBytes = 4; blockDim = 1; break;   // 50 = 8_8_8_8_AS_16_16_16_16
    case 54: df = DXGI_FORMAT_R10G10B10A2_UNORM; blockBytes = 4; blockDim = 1; break;   // GoWJ: 2_10_10_10_AS_16_16_16_16 (mismos bits que 7)
    case 51: df = DXGI_FORMAT_BC1_UNORM; blockBytes = 8; blockDim = 4; break;           // DXT1_AS_16_16_16_16
    case 52: df = DXGI_FORMAT_BC2_UNORM; blockBytes = 16; blockDim = 4; break;          // DXT2_3_AS_16_16_16_16
    case 53: df = DXGI_FORMAT_BC3_UNORM; blockBytes = 16; blockDim = 4; break;          // DXT4_5_AS_16_16_16_16
    case 2: df = DXGI_FORMAT_R8_UNORM; blockBytes = 1; blockDim = 1; break;
    case 18: df = DXGI_FORMAT_BC1_UNORM; blockBytes = 8; blockDim = 4; break;
    case 19: df = DXGI_FORMAT_BC2_UNORM; blockBytes = 16; blockDim = 4; break;
    case 20: df = DXGI_FORMAT_BC3_UNORM; blockBytes = 16; blockDim = 4; break;
    // Sesion 19: mas formatos (tabla de formatos de Xenia)
    case 3: df = DXGI_FORMAT_B5G5R5A1_UNORM; blockBytes = 2; blockDim = 1; break;      // k_1_5_5_5
    case 4: df = DXGI_FORMAT_B5G6R5_UNORM; blockBytes = 2; blockDim = 1; break;        // k_5_6_5
    case 15: df = DXGI_FORMAT_B4G4R4A4_UNORM; blockBytes = 2; blockDim = 1; break;     // k_4_4_4_4
    case 10: df = DXGI_FORMAT_R8G8_UNORM; blockBytes = 2; blockDim = 1; break;         // k_8_8
    case 24: df = DXGI_FORMAT_R16_UNORM; blockBytes = 2; blockDim = 1; break;          // k_16
    case 30: df = DXGI_FORMAT_R16_FLOAT; blockBytes = 2; blockDim = 1; break;          // k_16_FLOAT
    case 7: df = DXGI_FORMAT_R10G10B10A2_UNORM; blockBytes = 4; blockDim = 1; break;   // k_2_10_10_10
    case 25: df = DXGI_FORMAT_R16G16_UNORM; blockBytes = 4; blockDim = 1; if (signedOrExp) { df = DXGI_FORMAT_R16G16_FLOAT; conv = 3; } break; // k_16_16
    case 31: df = DXGI_FORMAT_R16G16_FLOAT; blockBytes = 4; blockDim = 1; break;       // k_16_16_FLOAT
    case 36: df = DXGI_FORMAT_R32_FLOAT; blockBytes = 4; blockDim = 1; break;          // k_32_FLOAT
    case 22: df = DXGI_FORMAT_R32_FLOAT; blockBytes = 4; blockDim = 1; conv = 1; break; // k_24_8 (profundidad)
    case 23: df = DXGI_FORMAT_R32_FLOAT; blockBytes = 4; blockDim = 1; conv = 2; break; // k_24_8_FLOAT
    case 26: df = DXGI_FORMAT_R16G16B16A16_UNORM; blockBytes = 8; blockDim = 1; if (signedOrExp) { df = DXGI_FORMAT_R16G16B16A16_FLOAT; conv = 3; } break; // k_16_16_16_16
    case 32: df = DXGI_FORMAT_R16G16B16A16_FLOAT; blockBytes = 8; blockDim = 1; break; // k_16_16_16_16_FLOAT
    // _EXPAND (27-29): mismos bits que los _FLOAT (Xenia los sube como half). NG2 lee asi el bufer HDR de la escena (sesion 9)
    case 27: df = DXGI_FORMAT_R16_FLOAT; blockBytes = 2; blockDim = 1; break;          // k_16_EXPAND
    case 28: df = DXGI_FORMAT_R16G16_FLOAT; blockBytes = 4; blockDim = 1; break;       // k_16_16_EXPAND
    case 29: df = DXGI_FORMAT_R16G16B16A16_FLOAT; blockBytes = 8; blockDim = 1; break; // k_16_16_16_16_EXPAND
    case 37: df = DXGI_FORMAT_R32G32_FLOAT; blockBytes = 8; blockDim = 1; break;       // k_32_32_FLOAT
    case 38: df = DXGI_FORMAT_R32G32B32A32_FLOAT; blockBytes = 16; blockDim = 1; break; // k_32_32_32_32_FLOAT
    case 49: df = DXGI_FORMAT_BC5_UNORM; blockBytes = 16; blockDim = 4; break;         // k_DXN
    case 59: df = DXGI_FORMAT_BC4_UNORM; blockBytes = 8; blockDim = 4; break;          // k_DXT5A
    default:
    {
        static std::unordered_map<uint64_t, int> seen;
        if (seen.size() < 64 && seen[(uint64_t(fmt) << 40) | (uint64_t(w) << 20) | h]++ == 0)
            LOG("[d3d12] formato de textura %u sin soporte (%ux%u, base %08X, frame %llu)", fmt, w, h, base, (unsigned long long)g_frameCounter);
        return false;
    }
    }
    // Profundidad resuelta por GPU leida como k_24_8 (R32_FLOAT) o como 8888 (bits empaquetados)
    // GoWJ sesion 10: tambien k_24_8_FLOAT (23). La profundidad de la escena que leen las particulas y la niebla con fundido por
    // profundidad (soft particles) es 23: caia a la lectura de memoria (sin el resolve, basura) -> niebla opaca que sigue a la
    // camara y el arma azul. GOWJ_NODEPTH23=1 = antes.
    static const bool noDepth23 = getenv("GOWJ_NODEPTH23") != nullptr;
    const bool zFmt = fmt == 22 || (fmt == 23 && !noDepth23);
    if (zFmt || fmt == 6)
    {
        auto last = g_resolvedDepthLast.find(base);
        auto it = (last != g_resolvedDepthLast.end() && last->second == ResolvedKey(base, w, h)) ? g_resolvedDepth.find(last->second) : g_resolvedDepth.end();
        if (fmt == 23)
        {   // diagnostico (sesion 10): de donde sale la profundidad k_24_8_FLOAT
            static std::unordered_map<uint32_t, int> seen23;
            if (seen23.size() < 16 && seen23[base]++ == 0)
            {
                auto r2 = last != g_resolvedDepthLast.end() ? g_resolvedDepth.find(last->second) : g_resolvedDepth.end();
                auto rc = g_resolvedLast.find(base);
                LOG("[d3d12] profundidad f23 %08X %ux%u: resolve de profundidad %s (%ux%u), resolve de color %s", base, w, h,
                    last == g_resolvedDepthLast.end() ? "no" : (it != g_resolvedDepth.end() ? "mismo tamano" : "otro tamano"),
                    r2 != g_resolvedDepth.end() ? r2->second.w : 0, r2 != g_resolvedDepth.end() ? r2->second.h : 0, rc != g_resolvedLast.end() ? "si" : "no");
            }
        }
        if (it != g_resolvedDepth.end() && it->second.rz && g_frameCounter - it->second.frame <= 60)
        {
            // Diferencia de endian entre resolve y lectura = permutacion de bytes (XOR del indice): 8in16 = 1, 8in32 = 3, 16in32 = 2
            static const uint32_t emask[4] = { 0, 1, 3, 2 };
            uint32_t xm = emask[it->second.endian & 3] ^ emask[endian & 3];
            ResolvedDepth& r = it->second;
            if (r.state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
            {
                D3D12_RESOURCE_BARRIER bs[2] = { Transition(r.rz.Get(), r.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE), Transition(r.rp.Get(), r.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) };
                ProbedBarrier(2, bs, __LINE__); r.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            }
            uint32_t swz = (fc[3] >> 1) & 0xFFF;
            uint32_t xmUse = zFmt ? 0 : xm;
            auto map = [xmUse, zFmt](uint32_t c) -> UINT { return c < 4 ? (zFmt ? 0u : (c ^ xmUse)) : (c == 4 ? D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0 : D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1); };
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = zFmt ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
            sd.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(map(swz & 7), map((swz >> 3) & 7), map((swz >> 6) & 7), map((swz >> 9) & 7));
            g_dev->CreateShaderResourceView(zFmt ? r.rz.Get() : r.rp.Get(), &sd, slot);
            g_bindSx = r.sx; g_bindSy = r.sy;
            return true;
        }
        static const bool noDerivedDepth = getenv("GOWJ_NODERIVEDDEPTH") != nullptr;
        if (zFmt && !noDerivedDepth && last != g_resolvedDepthLast.end())
        {
            auto di = g_resolvedDepth.find(last->second);
            if (di != g_resolvedDepth.end() && di->second.rz && di->second.w <= w && di->second.h <= h && g_frameCounter - di->second.frame <= 60)
            {
                ResolvedDepth& r = di->second;
                DerivedDepth& dd = g_derivedDepth[(uint64_t(base) << 26) ^ (uint64_t(w) << 13) ^ h];
                if (!dd.res || dd.w != w * r.sx || dd.h != h * r.sy)
                {
                    if (dd.res) g_deferredRelease[g_fi].push_back(dd.res);
                    dd = DerivedDepth{};
                    D3D12_HEAP_PROPERTIES def{ D3D12_HEAP_TYPE_DEFAULT };
                    D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = w * r.sx; td.Height = h * r.sy; td.DepthOrArraySize = 1; td.MipLevels = 1;
                    td.Format = DXGI_FORMAT_R32_FLOAT; td.SampleDesc.Count = 1;
                    if (FAILED(g_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&dd.res)))) dd.res.Reset();
                    else { dd.w = w * r.sx; dd.h = h * r.sy; dd.st = D3D12_RESOURCE_STATE_COPY_DEST;
                           static int nl = 0; if (nl++ < 8) LOG("[d3d12] profundidad %08X leida como %ux%u desde un resolve de %ux%u: copia derivada en la GPU", base, w, h, r.w, r.h); }
                }
                if (dd.res)
                {
                    if (dd.gen != r.gen)
                    {
                        D3D12_RESOURCE_BARRIER bs[2]; UINT nb = 0;
                        if (r.state != D3D12_RESOURCE_STATE_COPY_SOURCE)
                        {   // rz y rp comparten el estado
                            D3D12_RESOURCE_BARRIER b2[2] = { Transition(r.rz.Get(), r.state, D3D12_RESOURCE_STATE_COPY_SOURCE), Transition(r.rp.Get(), r.state, D3D12_RESOURCE_STATE_COPY_SOURCE) };
                            ProbedBarrier(2, b2, __LINE__); r.state = D3D12_RESOURCE_STATE_COPY_SOURCE;
                        }
                        if (dd.st != D3D12_RESOURCE_STATE_COPY_DEST) { bs[nb++] = Transition(dd.res.Get(), dd.st, D3D12_RESOURCE_STATE_COPY_DEST); dd.st = D3D12_RESOURCE_STATE_COPY_DEST; }
                        if (nb) ProbedBarrier(nb, bs, __LINE__);
                        D3D12_BOX box{ 0, 0, 0, r.w * r.sx, r.h * r.sy, 1 };
                        D3D12_TEXTURE_COPY_LOCATION dl{ dd.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; dl.SubresourceIndex = 0;
                        D3D12_TEXTURE_COPY_LOCATION sl{ r.rz.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; sl.SubresourceIndex = 0;
                        g_list->CopyTextureRegion(&dl, 0, 0, 0, &sl, &box);
                        dd.gen = r.gen;
                    }
                    if (dd.st != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) { auto b = Transition(dd.res.Get(), dd.st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); ProbedBarrier(1, &b, __LINE__); dd.st = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; }
                    uint32_t swz = (fc[3] >> 1) & 0xFFF;
                    auto map = [](uint32_t c) -> UINT { return c < 4 ? 0u : (c == 4 ? D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0 : D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1); };
                    D3D12_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = DXGI_FORMAT_R32_FLOAT; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
                    sd.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(map(swz & 7), map((swz >> 3) & 7), map((swz >> 6) & 7), map((swz >> 9) & 7));
                    g_dev->CreateShaderResourceView(dd.res.Get(), &sd, slot);
                    g_bindSx = r.sx; g_bindSy = r.sy;
                    return true;
                }
            }
        }
    }
    // Sesion 24: texturas resueltas por GPU en k_8 (copia 8888, canal rojo) o k_32_FLOAT (copia R32_FLOAT); mismo tamano exacto
    // Sesion 16 de NG2: tambien k_16_16_16_16_FLOAT y _EXPAND (mismos bits) desde el resolve por GPU de la escena HDR
    const uint32_t rfmt = fmt == 29 ? 32u : fmt == 54 ? 7u : fmt == 28 ? 31u : fmt;   // 28 = 16_16_EXPAND (mismos bits que 16_16_FLOAT)   // GoWJ sesion 8: 54 = mismos bits que 7 (escena 7e3 por GPU)
    if (fmt == 2 || fmt == 36 || (fmt == 25 && !signedOrExp) || fmt == 32 || fmt == 29 || fmt == 4 || ((fmt == 7 || fmt == 54 || fmt == 26) && !signedOrExp) || fmt == 31 || fmt == 28)   // half float: el signo y exp_adjust no se aplican (como al subir desde memoria)
    {
        uint64_t k = ResolvedKey(base, w, h) ^ ResolvedFmtTag(rfmt);
        auto last = g_resolvedLast.find(base);
        const bool exactHit = last != g_resolvedLast.end() && last->second != k && ExactRecentResolve(base, w, h, rfmt) != g_resolved.end();   // sesion 10
        // Sesion 19 (2): k_16_16_16_16_FLOAT leida con unos pocos texeles menos que la copia (281 -> 280): se enlaza la copia de la GPU
        // tal cual (la region derivada no funciona con 8 bytes por pixel y el volcado a memoria esperaba a la GPU en cada frame)
        static const bool noNear32 = getenv("GOWJ_NONEAR32") != nullptr;
        if (!exactHit && rfmt == 32 && !noNear32 && last != g_resolvedLast.end() && last->second != k)
        {
            auto nIt = g_resolved.find(last->second);
            if (nIt != g_resolved.end() && nIt->second.res && nIt->second.texFmt == 32 && w <= nIt->second.w && h <= nIt->second.h &&
                w + 8 >= nIt->second.w && h + 8 >= nIt->second.h && ResolveMemOk(nIt->second, base))
            {
                ResolvedTex& r = nIt->second;
                if (r.state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
                { auto b = Transition(r.res.Get(), r.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); ProbedBarrier(1, &b, __LINE__); r.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; }
                uint32_t swz = (fc[3] >> 1) & 0xFFF;
                auto map = [&](uint32_t c) -> UINT { return c < 4 ? HostRep(fmt, c) : (c == 4 ? D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0 : D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1); };
                D3D12_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = r.fmt; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
                sd.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(map(swz & 7), map((swz >> 3) & 7), map((swz >> 6) & 7), map((swz >> 9) & 7));
                g_dev->CreateShaderResourceView(r.res.Get(), &sd, slot);
                g_bindSx = r.sx; g_bindSy = r.sy;
                return true;
            }
        }
        if (!exactHit && last != g_resolvedLast.end() && last->second != k)
        {
            // Sesion 24: el resolve tiene otro tamano (p. ej. 112x207 leido como 110x207): copia de la region a una textura derivada
            // en la GPU (antes se volcaba a memoria esperando a la GPU: tirones de ~50 ms)
            auto srcIt = g_resolved.find(last->second);
            if (rfmt != 32 && rfmt != 4 && srcIt != g_resolved.end() && srcIt->second.res && srcIt->second.texFmt == rfmt && DynResFits(w, h, srcIt->second.w, srcIt->second.h) &&
                ResolveMemOk(srcIt->second, base))
            {
                ResolvedTex& src = srcIt->second;
                if (fmt == 2)
                {   // diagnostico (sesion 10)
                    static std::unordered_map<uint64_t, int> seenK8d;
                    if (seenK8d.size() < 32 && seenK8d[k]++ == 0)
                        LOG("[k8] %08X %ux%u servida como region de un resolve de %ux%u (frame %llu, ahora %llu)", base, w, h, src.w, src.h, (unsigned long long)src.frame, (unsigned long long)g_frameCounter);
                }
                ResolvedTex& dsub = g_resolved[k ^ 1];
                if (!dsub.res || dsub.gen != src.gen || dsub.w != w || dsub.h != h || dsub.fmt != src.fmt || dsub.sx != src.sx || dsub.sy != src.sy)
                {
                    if (!dsub.res || dsub.w != w || dsub.h != h || dsub.fmt != src.fmt || dsub.sx != src.sx || dsub.sy != src.sy)
                    {
                        if (dsub.res) g_deferredRelease[g_fi].push_back(dsub.res);
                        dsub = ResolvedTex{};
                        D3D12_HEAP_PROPERTIES def{ D3D12_HEAP_TYPE_DEFAULT };
                        D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = w * src.sx; td.Height = h * src.sy; td.DepthOrArraySize = 1; td.MipLevels = 1;
                        td.Format = src.fmt; td.SampleDesc.Count = 1;
                        g_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&dsub.res));
                        dsub.w = w; dsub.h = h; dsub.sx = src.sx; dsub.sy = src.sy; dsub.fmt = src.fmt; dsub.state = D3D12_RESOURCE_STATE_COPY_DEST;
                    }
                    if (dsub.res)
                    {
                        D3D12_RESOURCE_BARRIER bs[2]; UINT nb = 0;
                        if (src.state != D3D12_RESOURCE_STATE_COPY_SOURCE) { bs[nb++] = Transition(src.res.Get(), src.state, D3D12_RESOURCE_STATE_COPY_SOURCE); src.state = D3D12_RESOURCE_STATE_COPY_SOURCE; }
                        if (dsub.state != D3D12_RESOURCE_STATE_COPY_DEST) { bs[nb++] = Transition(dsub.res.Get(), dsub.state, D3D12_RESOURCE_STATE_COPY_DEST); dsub.state = D3D12_RESOURCE_STATE_COPY_DEST; }
                        if (nb) ProbedBarrier(nb, bs, __LINE__);
                        D3D12_BOX box{ 0, 0, 0, std::min(w, src.w) * src.sx, std::min(h, src.h) * src.sy, 1 };
                        SafeCopyRegion(dsub.res.Get(), src.res.Get(), box, "region derivada");
                        dsub.texFmt = src.texFmt; dsub.gen = src.gen; dsub.memHash = src.memHash; dsub.inMemory = src.inMemory; dsub.wsig = src.wsig; dsub.wlen = src.wlen; dsub.destBase = src.destBase; dsub.frame = src.frame;
                    }
                }
                if (dsub.res)
                {
                    if (dsub.state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
                    {
                        auto b = Transition(dsub.res.Get(), dsub.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); ProbedBarrier(1, &b, __LINE__);
                        dsub.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                    }
                    uint32_t swz = (fc[3] >> 1) & 0xFFF;
                    auto map = [&](uint32_t c) -> UINT { return c < 4 ? HostRep(fmt, c) : (c == 4 ? D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0 : D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1); };
                    D3D12_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = dsub.fmt; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
                    sd.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(map(swz & 7), map((swz >> 3) & 7), map((swz >> 6) & 7), map((swz >> 9) & 7));
                    g_dev->CreateShaderResourceView(dsub.res.Get(), &sd, slot);
                    g_bindSx = dsub.sx; g_bindSy = dsub.sy;
                    return true;
                }
            }
        }
        if (last != g_resolvedLast.end() && (last->second == k || exactHit))
        {
            auto it = g_resolved.find(k);
            if (fmt == 2 && it != g_resolved.end())
            {   // diagnostico (sesion 10): k_8 servida desde un resolve por GPU (sospecha: cache de glifos en memoria reutilizada)
                static std::unordered_map<uint64_t, int> seenK8;
                if (seenK8.size() < 32 && seenK8[k]++ == 0)
                    LOG("[k8] %08X %ux%u servida desde un resolve de la GPU (frame del resolve %llu, ahora %llu, en memoria %d, huella igual %d)", base, w, h,
                        (unsigned long long)it->second.frame, (unsigned long long)g_frameCounter, (int)it->second.inMemory, (int)(MemHash(base) == it->second.memHash));
            }
            if (it != g_resolved.end() && it->second.res && it->second.texFmt == rfmt && ResolveMemOk(it->second, base))
            {
                ResolvedTex& r = it->second;
                if (r.state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
                {
                    auto b = Transition(r.res.Get(), r.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); ProbedBarrier(1, &b, __LINE__);
                    r.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                }
                // Un solo canal (R del render target): todos los canales de la textura lo replican (RRRR, como HostRep)
                uint32_t swz = (fc[3] >> 1) & 0xFFF;
                const bool xs = rfmt == 4 && r.swapRB;   // 5_6_5 por GPU: la copia guarda los colores del RT; el swap se aplica al leer
                auto map = [&](uint32_t c) -> UINT { if (c < 4) { UINT h = HostRep(fmt, c); return xs && (h == 0 || h == 2) ? 2 - h : h; } return c == 4 ? D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0 : D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1; };
                D3D12_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = r.fmt; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
                sd.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(map(swz & 7), map((swz >> 3) & 7), map((swz >> 6) & 7), map((swz >> 9) & 7));
                g_dev->CreateShaderResourceView(r.res.Get(), &sd, slot);
                g_bindSx = r.sx; g_bindSy = r.sy;
                return true;
            }
        }
    }
    // Textura producida por un resolve por GPU: se usa directamente (canales: si el resolve intercambio R/B, se deshace en el SRV)
    // GoWJ sesion 2: la escena se resuelve en 2_10_10_10 (fmt 7) y el tonemap la lee como fmt 54 (mismos bits): el resolve por GPU
    // la guarda como 8888 (texFmt 6) y se enlaza igual (antes: formato 54 sin soporte -> textura blanca -> imagen negra).
    bool as8888 = fmt == 6;
    static const bool as54 = getenv("GOWJ_54AS8888") != nullptr;   // sesion 8: getenv fuera del camino caliente
    if ((fmt == 7 || fmt == 54) && as54)
    {
        auto l2 = g_resolvedLast.find(base);
        auto i2 = l2 != g_resolvedLast.end() ? g_resolved.find(l2->second) : g_resolved.end();
        as8888 = i2 != g_resolved.end() && i2->second.res && i2->second.texFmt == 6 && !i2->second.inMemory;
    }
    if (as8888)
    {
        auto last = g_resolvedLast.find(base);
        auto it = (last != g_resolvedLast.end() && last->second == ResolvedKey(base, w, h)) ? g_resolved.find(last->second) : g_resolved.end();
        if (last != g_resolvedLast.end() && it == g_resolved.end()) it = ExactRecentResolve(base, w, h, 6);   // sesion 10
        if (last != g_resolvedLast.end() && it == g_resolved.end())
        {
            // El ultimo resolve a esta direccion tiene otro tamano (p. ej. 224x416 leido como 220x415):
            // se copia la region que se pide a una textura de ese tamano, en la GPU
            auto srcIt = g_resolved.find(last->second);
            static bool noDsub = getenv("GOWJ_NODSUB") != nullptr;
            if (!noDsub && srcIt != g_resolved.end() && srcIt->second.res && srcIt->second.texFmt == 6 && DynResFits(w, h, srcIt->second.w, srcIt->second.h))
            {
                ResolvedTex& s = srcIt->second;
                ResolvedTex& dsub = g_resolved[ResolvedKey(base, w, h) ^ 1];   // clave derivada (bit 0 distinto)
                static bool genMode = getenv("GOWJ_DSUBFRAME") == nullptr;   // GOWJ_DSUBFRAME=1: criterio anterior (por frame)
                if (!dsub.res || (genMode ? dsub.gen != s.gen : dsub.frame != s.frame) || dsub.w != w || dsub.h != h || dsub.sx != s.sx || dsub.sy != s.sy)
                {
                    if (!dsub.res || dsub.w != w || dsub.h != h || dsub.sx != s.sx || dsub.sy != s.sy)
                    {
                        if (dsub.res) g_deferredRelease[g_fi].push_back(dsub.res);
                        dsub = ResolvedTex{};
                        D3D12_HEAP_PROPERTIES def{ D3D12_HEAP_TYPE_DEFAULT };
                        D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = w * s.sx; td.Height = h * s.sy; td.DepthOrArraySize = 1; td.MipLevels = 1;
                        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
                        g_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&dsub.res));
                        dsub.w = w; dsub.h = h; dsub.sx = s.sx; dsub.sy = s.sy; dsub.state = D3D12_RESOURCE_STATE_COPY_DEST;
                    }
                    if (dsub.res)
                    {
                        D3D12_RESOURCE_BARRIER bs[2]; UINT nb = 0;
                        if (s.state != D3D12_RESOURCE_STATE_COPY_SOURCE) { bs[nb++] = Transition(s.res.Get(), s.state, D3D12_RESOURCE_STATE_COPY_SOURCE); s.state = D3D12_RESOURCE_STATE_COPY_SOURCE; }
                        if (dsub.state != D3D12_RESOURCE_STATE_COPY_DEST) { bs[nb++] = Transition(dsub.res.Get(), dsub.state, D3D12_RESOURCE_STATE_COPY_DEST); dsub.state = D3D12_RESOURCE_STATE_COPY_DEST; }
                        if (nb) ProbedBarrier(nb, bs, __LINE__);
                        D3D12_BOX box{ 0, 0, 0, std::min(w, s.w) * s.sx, std::min(h, s.h) * s.sy, 1 };
                        SafeCopyRegion(dsub.res.Get(), s.res.Get(), box, "region derivada");
                        dsub.swapRB = s.swapRB; dsub.endian = s.endian; dsub.frame = s.frame; dsub.memHash = s.memHash; dsub.inMemory = s.inMemory; dsub.wsig = s.wsig; dsub.wlen = s.wlen; dsub.destBase = s.destBase; dsub.gen = s.gen;
                    }
                }
                if (dsub.res) it = g_resolved.find(ResolvedKey(base, w, h) ^ 1);
            }
        }
        // Sesion 23: sin limite de antiguedad (el juego reutiliza copias de hace muchos frames, p. ej. los retratos del lobby);
        // solo se descarta si la memoria de destino ha cambiado desde el resolve (la CPU la ha reescrito).
        {   // GOWJ_TEXAGE=<direccion hex>: antiguedad del resolve que se usa al leer esa direccion (diagnostico, sesion 24)
            static uint32_t ageAddr = getenv("GOWJ_TEXAGE") ? (uint32_t)strtoul(getenv("GOWJ_TEXAGE"), nullptr, 16) : 0;
            static uint32_t ageLogs = 0;
            if (ageAddr && base == ageAddr && ageLogs++ < 400)
                LOG("[texage] %08X %ux%u: frame %llu, resolve del frame %llu (gen %llu), encontrado %d, memoria igual %d, ultimo resolve clave igual %d",
                    base, w, h, (unsigned long long)g_frameCounter, it != g_resolved.end() ? (unsigned long long)it->second.frame : 0ull,
                    it != g_resolved.end() ? (unsigned long long)it->second.gen : 0ull, it != g_resolved.end(),
                    it != g_resolved.end() ? (int)ResolveMemOk(it->second, base) : -1,
                    last != g_resolvedLast.end() ? (int)(last->second == ResolvedKey(base, w, h)) : -1);
        }
        if (it != g_resolved.end() && it->second.res && ResolveMemOk(it->second, base))
        {
            static const uint32_t emask[4] = { 0, 1, 3, 2 };
            uint32_t xm = emask[it->second.endian & 3] ^ emask[endian & 3];
            ResolvedTex& r = it->second;
            if (r.state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
            {
                auto b = Transition(r.res.Get(), r.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); ProbedBarrier(1, &b, __LINE__);
                r.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            }
            // Canal de la textura -> byte de la palabra (permutacion de endian) -> canal del RT (vista BGRA: x=R, y=G, z=B, w=A)
            const UINT chan[4] = { r.swapRB ? 2u : 0u, 1u, r.swapRB ? 0u : 2u, 3u };
            uint32_t swz = (fc[3] >> 1) & 0xFFF;
            auto map = [&](uint32_t c) -> UINT { return c < 4 ? chan[c ^ xm] : (c == 4 ? D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0 : D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1); };
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
            sd.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(map(swz & 7), map((swz >> 3) & 7), map((swz >> 6) & 7), map((swz >> 9) & 7));
            g_dev->CreateShaderResourceView(r.res.Get(), &sd, slot);
            g_bindSx = r.sx; g_bindSy = r.sy;
            return true;
        }
        if (last != g_resolvedLast.end() && it == g_resolved.end())
        {
            static std::unordered_map<uint32_t, int> seen;
            if (seen[base]++ == 0) LOG("[d3d12] textura resuelta por GPU en %08X leida con otro tamano: %ux%u endian %u", base, w, h, endian);
        }
    }
    uint32_t bw = (w + blockDim - 1) / blockDim, bh = (h + blockDim - 1) / blockDim;
    uint32_t pitchBlocks = std::max<uint32_t>((pitchTexels ? pitchTexels : w) / blockDim, bw);
    uint32_t log2b = blockBytes == 1 ? 0 : blockBytes == 2 ? 1 : blockBytes == 4 ? 2 : blockBytes == 8 ? 3 : 4;
    const uint8_t* src = g_base + (0xA0000000u | (base & 0x1FFFFFFF));
    // hash rapido de los datos para detectar cambios
    // Sesion 24 (rendimiento): la huella se calcula una vez por frame y textura (era ~20 % del hilo de la GPU: hasta 64 KB por
    // bind en cada draw). Se invalida en cada resolve por CPU. GOWJ_TEXHASHEVERY=1 la recalcula siempre.
    static bool hashEvery = getenv("GOWJ_TEXHASHEVERY") != nullptr;
    struct HashMemo { uint64_t frame, rcount, dh; };
    static std::unordered_map<uint64_t, HashMemo> hashMemo;
    uint64_t memoKey = TexKey(base, fmt, w, h, pitchBlocks * 2 + (tiled ? 1 : 0), 9);
    // GoWJ sesion 3: la cache de glifos (k_8) se rellena entre draws del mismo cuadro: sin memo para k_8 (texto corrupto si no)
    static const bool k8Memo = getenv("GOWJ_K8MEMO") != nullptr;
    const bool noMemo = hashEvery || (fmt == 2 && !k8Memo);
    auto memoIt = noMemo ? hashMemo.end() : hashMemo.find(memoKey);
    bool memoHit = memoIt != hashMemo.end() && memoIt->second.frame == g_frameCounter && memoIt->second.rcount == g_cpuResolveCounter;
    // GoWJ sesion 10: paginas sucias (texdirty.cpp, como en Gears 3). Texturas de mas de 64 KB (huella muestreada) y mipmaps (antes
    // sin comprobar): si ninguna pagina se ha escrito desde que se subio, se reutiliza sin huella; si se escribio, se vuelve a subir
    // aunque la huella muestreada coincida (fuentes e iconos casi vacios: letras de trozos de otras). GOWJ_NOTEXDIRTY=1 = antes.
    static const bool dirtyOn = TexDirtyEnabled() && !hashEvery;
    struct DirtyMemo { uint64_t dh; uint64_t rcount; uint64_t sigBase, sigMips; };
    static std::unordered_map<uint64_t, DirtyMemo> dirtyMemo;
    static uint64_t dirtyUniq = 0;
    const uint32_t texBytesD = (uint32_t)std::min<uint64_t>(uint64_t((pitchBlocks + 31) & ~31u) * ((bh + 31) & ~31u) * blockBytes, 16u << 20);
    const uint32_t mipAD = fc[5] & 0xFFFFF000, mipBytesD = (mipAD && ((fc[4] >> 6) & 0xF)) ? texBytesD / 2 + 0x1000 : 0;
    // sesion 10 (rendimiento): tambien las texturas pequenas (antes huella completa en cada cuadro). GOWJ_SMALLTEXHASH=1 = antes.
    static const bool smallHash = getenv("GOWJ_SMALLTEXHASH") != nullptr;
    const bool bigTexD = texBytesD > 65536 || !smallHash;
    bool forceNew = false;
    auto dirtyIt = dirtyOn ? dirtyMemo.find(memoKey) : dirtyMemo.end();
    if (dirtyIt != dirtyMemo.end())
    {
        bool baseDirty = bigTexD && !TexDirtyValid(base, texBytesD, dirtyIt->second.sigBase), mipsDirty = mipBytesD && !TexDirtyValid(mipAD, mipBytesD, dirtyIt->second.sigMips);
        if (!baseDirty && !mipsDirty && dirtyIt->second.rcount == g_cpuResolveCounter && bigTexD) { memoHit = true; memoIt = hashMemo.end(); }
        else if (baseDirty || mipsDirty) { forceNew = true; memoHit = false; }
    }
    uint64_t dh = memoHit ? (memoIt != hashMemo.end() ? memoIt->second.dh : dirtyIt->second.dh) : 1469598103934665603ull;
    if (!memoHit) {
    for (uint32_t i = 0; i < 64; i++) { dh ^= ((const uint32_t*)src)[i * 37 % 4096]; dh *= 1099511628211ull; }
    // Sesion 24: ademas, muestras repartidas por toda la textura. Con solo los primeros 16 KB, una copia cuya parte de arriba
    // no cambia (la SSAO de 640x360 en k_8, con el fondo constante) no se volvia a subir.
    static bool oldTexHash = getenv("GOWJ_TEXHASHOLD") != nullptr;
    if (!oldTexHash)
    {
        uint64_t bytes = uint64_t((pitchBlocks + 31) & ~31u) * ((bh + 31) & ~31u) * blockBytes;
        uint32_t dwords = (uint32_t)std::min<uint64_t>(bytes / 4, 1u << 22);
        // GoWJ sesion 2: la cache de glifos de Scaleform/UE3 es una k_8 grande (fmt 2) que la CPU va rellenando: con 256 muestras no se
        // veian los glifos nuevos (texto corrupto en la pantalla de carga). Para k_8 de hasta 4 MB, todos los dwords.
        static const bool noFullK8 = getenv("GOWJ_NOK8FULLHASH") != nullptr;
        static const bool slowHash = getenv("GOWJ_TEXHASHSLOW") != nullptr;   // sesion 10: huella de un carril (antes)
        if (dwords <= 16384 || (!noFullK8 && fmt == 2 && dwords <= (1u << 20)))   // texturas pequenas (<= 64 KB) o k_8: todos los dwords
        {
            if (!slowHash) dh = HashDwordsFast((const uint32_t*)src, dwords, dh);
            else for (uint32_t i = 0; i < dwords; i++) { dh ^= ((const uint32_t*)src)[i]; dh *= 1099511628211ull; }
        }
        else
            for (uint32_t i = 0; i < 256; i++) { dh ^= ((const uint32_t*)src)[(uint64_t(i) * dwords / 256 + (i * 977) % 61) % dwords]; dh *= 1099511628211ull; }
    }
    if (forceNew) dh ^= (++dirtyUniq) * 0x9E3779B97F4A7C15ull;   // contenido nuevo seguro aunque la huella coincida
    if (!noMemo) { if (hashMemo.size() > 65536) hashMemo.clear(); hashMemo[memoKey] = { g_frameCounter, g_cpuResolveCounter, dh }; }
    if (dirtyOn && (bigTexD || mipBytesD))
    {   // vigilar desde ahora (antes de leer los datos para subirlos: una escritura posterior marcara la pagina)
        uint64_t sb = bigTexD ? TexDirtyArmSig(base, texBytesD) : 0, sm = mipBytesD ? TexDirtyArmSig(mipAD, mipBytesD) : 0;
        if (dirtyMemo.size() > 65536) dirtyMemo.clear();
        dirtyMemo[memoKey] = { dh, g_cpuResolveCounter, sb, sm };
    }
    }
    // Sesion 24: mipmaps. dword 5 bits 12-31 = direccion de los mips, dword 4 bits 6-9 = nivel maximo (como Xenia)
    auto l2c = [](uint32_t v) { uint32_t l = 0; while ((1u << l) < v) l++; return l; };
    auto l2f = [](uint32_t v) { uint32_t l = 0; while ((2u << l) <= v) l++; return l; };
    uint32_t mipAddr = fc[5] & 0xFFFFF000, mipMaxLvl = (fc[4] >> 6) & 0xF;
    bool packedMips = ((fc[5] >> 11) & 1) != 0;
    static bool noMips = getenv("GOWJ_NOMIPS") != nullptr;
    uint32_t levels = 1;
    if (!noMips && mipAddr && mipMaxLvl > 0 && conv != 1 && conv != 2)
        levels = std::min(mipMaxLvl, l2f(std::max(w, h))) + 1;
    // Desplazamiento (en bloques) de un nivel dentro del tile de mips empaquetados (GetPackedMipOffset de Xenia, 2D)
    auto packedOff = [&](uint32_t L, uint32_t& xb, uint32_t& yb) -> bool {
        uint32_t lw = l2c(w), lh = l2c(h), ls = std::min(lw, lh);
        xb = yb = 0;
        if (ls > 4 + L) return false;
        uint32_t pbase = ls > 4 ? ls - 4 : 0, pm = L - pbase, x = 0, y = 0;
        if (pm < 3) { if (lw > lh) y = 16u >> pm; else x = 16u >> pm; }
        else if (lw > lh) x = (1u << (lw - pbase)) >> (pm - 2);
        else y = (1u << (lh - pbase)) >> (pm - 2);
        xb = x / blockDim; yb = y / blockDim;
        return true;
    };
    // El formato DXGI va en la clave: la misma textura puede subirse como UNORM o como half segun el signo de la constante (sesion 23)
    uint64_t key = TexKey(base, fmt, w, h, (tiled ? 1u : 0u) | (uint32_t(df) << 8) | (levels << 20), 0);   // FNV: el XOR anterior solapaba texturas (GH sesion 24)
    TexEntry& e = g_textures[key];
    static bool texNoCacheAll = getenv("GOWJ_TEXNOCACHE") != nullptr;   // diagnostico: resube siempre (GOWJ_TEXNOCACHE=<fmt> solo ese formato)
    static int texNoCacheFmt = texNoCacheAll ? atoi(getenv("GOWJ_TEXNOCACHE")) : -1;
    bool texNoCache = texNoCacheAll && (texNoCacheFmt <= 1 || (int)fmt == texNoCacheFmt);
    static bool noResolveGen = getenv("GOWJ_NORESOLVEGEN") != nullptr;
    uint64_t rgen = 0;
    if (!noResolveGen) { auto gi = g_cpuResolveGen.find(base & 0xFFFFF000); if (gi != g_cpuResolveGen.end()) rgen = gi->second; }
    if (!e.res || e.dataHash != dh || texNoCache || e.resolveGen != rgen)
    {
        if (!e.res)
        {
            D3D12_HEAP_PROPERTIES def{ D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = bw * blockDim; td.Height = bh * blockDim;
            td.DepthOrArraySize = 1; td.MipLevels = (UINT16)levels; td.Format = df; td.SampleDesc.Count = 1;
            double tc0 = QpcMs();
            if (FAILED(CreateTextureResource(td, e.res))) return false;
            g_hTexCreateMs += QpcMs() - tc0; g_hTexNew++;
            e.fmt = df; e.levels = levels;
        }
        else { auto b = Transition(e.res.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST); ProbedBarrier(1, &b, __LINE__); }
        UINT rowPitch = (bw * blockBytes + 255) & ~255u;
        UINT64 o = RingAlloc(UINT64(rowPitch) * bh, 512);
        if (o == ~0ull) { auto bb = Transition(e.res.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); ProbedBarrier(1, &bb, __LINE__); return false; }   // anillo lleno: la textura vuelve a su estado (sesion 8)
        uint8_t* dst = g_ringMap[g_fi] + o;
        // Sesion 24: mips empaquetados (dword 5 bit 11): si el lado mas corto es <= 16 texeles, el nivel base esta desplazado 16 texeles
        // dentro del tile de 32x32 bloques (en Y si es mas ancha que alta, si no en X), como GetPackedMipOffset de Xenia.
        // Sin esto las texturas finas de la interfaz (barra del medidor 256x16, marcadores 32x16) salian como rectangulos.
        uint32_t pkX = 0, pkY = 0;
        {
            static bool noPacked = getenv("GOWJ_NOPACKEDMIPS") != nullptr;
            auto l2c = [](uint32_t v) { uint32_t l = 0; while ((1u << l) < v) l++; return l; };
            uint32_t lw = l2c(w), lh = l2c(h);
            if (!noPacked && ((fc[5] >> 11) & 1) && std::min(lw, lh) <= 4)
            { if (lw > lh) pkY = 16 / blockDim; else pkX = 16 / blockDim; }
        }
        // Destilea un nivel: lsrc = inicio del nivel, lpitch = pitch en bloques (alineado a 32 si es tiled), (lox, loy) = desplazamiento
        auto fillRows = [&](const uint8_t* src, uint32_t bw, uint32_t by0, uint32_t by1, uint32_t lpitch, uint32_t pkX, uint32_t pkY, uint8_t* dst, UINT rowPitch)
        {
        for (uint32_t by = by0; by < by1; by++)
        {
            uint32_t outer = tiled ? TiledOuter(by + pkY, lpitch, log2b) : 0;
            for (uint32_t bx = 0; bx < bw; bx++)
            {
                uint32_t so = tiled ? TiledInner(bx + pkX, by + pkY, log2b, outer) : ((by + pkY) * lpitch + bx + pkX) * blockBytes;
                if (blockBytes == 1) { dst[by * rowPitch + bx] = src[so]; continue; }
                if (blockBytes == 2)
                {
                    // Texel de 16 bits: se aplica el swap a la palabra de 32 bits que lo contiene y se extrae su mitad
                    uint32_t wd = Swap32(*(const uint32_t*)(src + (so & ~3u)), endian);
                    *(uint16_t*)(dst + by * rowPitch + bx * 2) = (uint16_t)(wd >> ((so & 2) * 8));
                    continue;
                }
                const uint32_t* sp = (const uint32_t*)(src + so);
                uint32_t* dp = (uint32_t*)(dst + by * rowPitch + bx * blockBytes);
                // Sesion 23 (rendimiento): se convierte en un bufer local y se escribe una sola vez. dst es memoria de subida (escritura
                // combinada): leerla de vuelta (DecodeDepth24(dp[0]), c16[k]) costaba ~180 ns por texel.
                uint32_t tmp[4];
                for (uint32_t k = 0; k < blockBytes / 4; k++) tmp[k] = Swap32(sp[k], endian);
                if (conv == 3)
                {
                    // Cada componente de 16 bits -> half, con su signo y el exp_adjust de la constante
                    uint16_t* c16 = (uint16_t*)tmp;
                    float mul = ldexpf(1.0f, expAdjust);
                    for (uint32_t k = 0; k < blockBytes / 2; k++)
                    {
                        bool sgn = ((signBits >> (k * 2)) & 3) == 1;
                        float f = sgn ? std::max(-1.0f, (int16_t)c16[k] / 32767.0f) : c16[k] / 65535.0f;
                        c16[k] = FloatToHalf(f * mul);
                    }
                }
                else if (conv)
                {
                    // 24 bits de profundidad en la parte alta, stencil en la baja
                    float z = DecodeDepth24(tmp[0] >> 8, conv == 2);
                    memcpy(tmp, &z, 4);
                }
                for (uint32_t k = 0; k < blockBytes / 4; k++) dp[k] = tmp[k];
            }
        }
        };
        // Sesion 23 (rendimiento): las texturas grandes se destilean por franjas de filas en varios hilos (los cortes de escena
        // subian decenas de texturas de golpe en el hilo de la GPU: tirones de 200-350 ms). GOWJ_TEXST=1 = un solo hilo.
        auto fillLevel = [&](const uint8_t* src, uint32_t bw, uint32_t bh, uint32_t lpitch, uint32_t pkX, uint32_t pkY, uint8_t* dst, UINT rowPitch)
        {
            static const bool st = getenv("GOWJ_TEXST") != nullptr;
            static const unsigned nthr = std::max(1u, std::min(8u, std::thread::hardware_concurrency() / 2));
            if (st || nthr < 2 || uint64_t(bw) * bh < 32768 || bh < 32) { fillRows(src, bw, 0, bh, lpitch, pkX, pkY, dst, rowPitch); return; }
            ParallelFor(nthr, [&](unsigned t) {
                uint32_t a = uint32_t(uint64_t(bh) * t / nthr), b = uint32_t(uint64_t(bh) * (t + 1) / nthr);
                fillRows(src, bw, a, b, lpitch, pkX, pkY, dst, rowPitch);
            });
        };
        double tf0 = QpcMs(); g_hTexUp++;
        fillLevel(src, bw, bh, tiled ? ((pitchBlocks + 31) & ~31u) : pitchBlocks, pkX, pkY, dst, rowPitch);
        struct FillTimer { double t; uint32_t base, fmt, w, h, lv; bool tl; ~FillTimer() { double d = QpcMs() - t; g_hTexFillMs += d;
            static int slow = 0; if (d > 8 && slow++ < 20) LOG("[d3d12] subida lenta: textura %08X formato %u %ux%u, %u niveles, %s: %.1f ms", base, fmt, w, h, lv, tl ? "tiled" : "lineal", d); } }
            fillTimer{ tf0, base, fmt, w, h, levels, tiled };
        // GoWJ sesion 2: GOWJ_TEXLIST=<segundos>: registra cada subida de textura a partir de ese momento (100 como maximo)
        if (static const char* tl = getenv("GOWJ_TEXLIST"); tl)
        {
            static DWORD t0l = GetTickCount(); static int nl = 0;
            if (GetTickCount() - t0l >= (DWORD)(atof(tl) * 1000) && nl++ < 150)
                LOG("[texlist] %08X fmt %u %ux%u niveles %u %s pitch %u", base, fmt, w, h, levels, tiled ? "tiled" : "lineal", pitchBlocks);
        }
        // GOWJ_TEXDUMP=<base hex>: guarda los datos ya destileados de esa textura (formato sin bloques) en texdump_*.raw (sesion 24)
        if (static const char* td = getenv("GOWJ_TEXDUMP"); td && (!strcmp(td, "K8") ? (fmt == 2 && w == 512 && h == 512) : (base & 0xFFFFF000) == ((uint32_t)strtoul(td, nullptr, 16) & 0xFFFFF000)))
        {
            static int dumps = 0;
            static int onlyFmt = getenv("GOWJ_TEXDUMPFMT") ? atoi(getenv("GOWJ_TEXDUMPFMT")) : -1;            // filtro de formato
            // desde ese frame, o t<segundos> desde el arranque
            static const char* tm = getenv("GOWJ_TEXDUMPMIN");
            static long long minFrame = (tm && tm[0] != 't') ? atoll(tm) : 0;
            static DWORD t0 = GetTickCount();
            bool timeOk = !(tm && tm[0] == 't') || GetTickCount() - t0 >= (DWORD)(atof(tm + 1) * 1000);
            if (dumps < 12 && (onlyFmt < 0 || (int)fmt == onlyFmt) && (long long)g_frameCounter >= minFrame && timeOk)
            {
                dumps++;
                char pn[128]; snprintf(pn, sizeof(pn), "texdump_%08X_f%u_%ux%u_bpp%u_%llu.raw", base, fmt, bw, bh, blockBytes, (unsigned long long)g_frameCounter);
                if (FILE* f = fopen(pn, "wb")) { for (uint32_t y = 0; y < bh; y++) fwrite(dst + y * rowPitch, 1, bw * blockBytes, f); fclose(f); }
                // GoWJ sesion 3: tambien los bytes crudos de memoria (pitch alineado a 32 x alto alineado a 32) y la constante de fetch
                snprintf(pn, sizeof(pn), "texraw_%08X_f%u_p%u_%ux%u_%08X_%08X_%08X.raw", base, fmt, pitchBlocks, bw, bh, fc[0], fc[3], fc[5]);
                if (FILE* f = fopen(pn, "wb")) { fwrite(src, 1, size_t((pitchBlocks + 31) & ~31u) * ((bh + 31) & ~31u) * blockBytes, f); fclose(f); }
            }
        }
        D3D12_TEXTURE_COPY_LOCATION d{ e.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; d.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION sl{ g_ring[g_fi].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        sl.PlacedFootprint.Offset = o; sl.PlacedFootprint.Footprint = { df, bw * blockDim, bh * blockDim, 1, rowPitch };
        g_list->CopyTextureRegion(&d, 0, 0, 0, &sl, nullptr);
        // Sesion 24: niveles 1..n desde mip_address (disposicion de GetGuestTextureLayout de Xenia): cada nivel ocupa el tamano de la
        // potencia de dos desplazada, alineado a 32x32 bloques y a 4 KB; desde el nivel empaquetado (lado corto <= 16) todos van en
        // un mismo tile con los desplazamientos de packedOff.
        if (levels > 1)
        {
            const uint8_t* msrc = g_base + (0xA0000000u | (mipAddr & 0x1FFFFFFF));
            uint32_t P = packedMips ? (l2c(std::min(w, h)) > 4 ? l2c(std::min(w, h)) - 4 : 0) : 99;
            uint32_t npw = 1u << l2c(w), nph = 1u << l2c(h);
            auto lvlPitch = [&](uint32_t l) { uint32_t t = std::max(npw >> l, 1u); uint32_t pb = (((t + blockDim - 1) / blockDim) + 31) & ~31u;
                if (!tiled) pb = ((pb * blockBytes + 255) & ~255u) / blockBytes; return pb; };
            auto lvlRows = [&](uint32_t l) { uint32_t t = std::max(nph >> l, 1u); return (((t + blockDim - 1) / blockDim) + 31) & ~31u; };
            auto lvlStride = [&](uint32_t l) { return (uint64_t(lvlPitch(l)) * blockBytes * lvlRows(l) + 4095) & ~4095ull; };
            for (uint32_t L = 1; L < levels; L++)
            {
                uint32_t stored = std::min(L, P);       // nivel cuyo almacenamiento contiene a L
                uint64_t off = 0;
                if (P != 0) for (uint32_t l = 1; l < stored; l++) off += lvlStride(l);
                uint32_t storeLvl = (P == 0) ? 0 : stored;
                uint32_t ox = 0, oy = 0;
                if (L >= P) packedOff(L, ox, oy);
                uint32_t lw = std::max(w >> L, 1u), lh = std::max(h >> L, 1u);
                uint32_t lbw = (lw + blockDim - 1) / blockDim, lbh = (lh + blockDim - 1) / blockDim;
                UINT lrp = (lbw * blockBytes + 255) & ~255u;
                UINT64 lo = RingAlloc(UINT64(lrp) * lbh, 512);
                if (lo == ~0ull) break;
                fillLevel(msrc + off, lbw, lbh, lvlPitch(storeLvl), ox, oy, g_ringMap[g_fi] + lo, lrp);
                if (static const char* tdm = getenv("GOWJ_TEXDUMP"); tdm && (tdm[0] == 'M' ? (int)fmt == atoi(tdm + 1) && w >= 128 && h >= 128 : (base & 0xFFFFF000) == ((uint32_t)strtoul(tdm, nullptr, 16) & 0xFFFFF000)))
                {   // diagnostico (sesion 10): cada nivel de mip ya destileado
                    static int mdumps = 0;
                    if (mdumps++ < 120)
                    {
                        LOG("[texdump] %08X fmt %u %ux%u nivel %u/%u: mips en %08X empaquetados %d P %u desplazamiento 0x%llX pitch %u bloques, pos (%u,%u) bloques, %ux%u bloques",
                            base, fmt, w, h, L, levels, mipAddr, (int)packedMips, P, (unsigned long long)off, lvlPitch(storeLvl), ox, oy, lbw, lbh);
                        char pn[128]; snprintf(pn, sizeof(pn), "texdump_%08X_f%u_L%u_%ux%u_bpp%u_%llu.raw", base, fmt, L, lbw, lbh, blockBytes, (unsigned long long)g_frameCounter);
                        if (FILE* f = fopen(pn, "wb")) { for (uint32_t y = 0; y < lbh; y++) fwrite(g_ringMap[g_fi] + lo + y * lrp, 1, lbw * blockBytes, f); fclose(f); }
                    }
                }
                D3D12_TEXTURE_COPY_LOCATION md{ e.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; md.SubresourceIndex = L;
                D3D12_TEXTURE_COPY_LOCATION ms{ g_ring[g_fi].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                ms.PlacedFootprint.Offset = lo; ms.PlacedFootprint.Footprint = { df, lbw * blockDim, lbh * blockDim, 1, lrp };
                g_list->CopyTextureRegion(&md, 0, 0, 0, &ms, nullptr);
            }
        }
        auto b = Transition(e.res.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        ProbedBarrier(1, &b, __LINE__);
        e.dataHash = dh; e.resolveGen = rgen;
        // Estadistica (sesion 21): texturas que se vuelven a subir mucho
        g_texUploads++; g_texUploadBytes += uint64_t(rowPitch) * bh;
        CmdTrace("tex2D %08X fmt %u %ux%u df %d bloques %ux%u rp %u off %llu", base, fmt, w, h, (int)df, bw, bh, rowPitch, (unsigned long long)o);
        static std::unordered_map<uint64_t, uint32_t> perTex;
        uint32_t c = ++perTex[key];
        if (c == 100) LOG("[d3d12] textura %08X (%ux%u formato %u) subida 100 veces (contenido cambiante)", base, w, h, fmt);
    }
    e.lastFrame = g_frameCounter;
    // swizzle de la constante (3 bits por componente: 0-3 = xyzw, 4 = 0, 5 = 1)
    uint32_t swz = (fc[3] >> 1) & 0xFFF;
    auto map = [fmt](uint32_t c) -> UINT { return c < 4 ? HostRep(fmt, c) : (c == 4 ? D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0 : D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1); };
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = e.fmt; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = e.levels ? e.levels : 1;
    {   // GoWJ sesion 10: mip_min_level (dword 4 bits 2-5). Mientras UE3 aun no ha cargado los mips grandes de una textura (streaming:
        // iconos que se cargan al momento, p. ej. el arma del aviso de "recoger"), limita el muestreo a los niveles ya cargados. Se
        // ignoraba y se leia el nivel 0 sin cargar (basura a bloques). GOWJ_NOMIPMIN=1 = antes.
        static const bool noMipMin = getenv("GOWJ_NOMIPMIN") != nullptr;
        uint32_t minLvl = (fc[4] >> 2) & 0xF, lv = e.levels ? e.levels : 1;
        if (!noMipMin && minLvl && lv > 1)
        {
            minLvl = std::min(minLvl, lv - 1);
            sd.Texture2D.MostDetailedMip = minLvl; sd.Texture2D.MipLevels = lv - minLvl;
            static int nl = 0; if (nl++ < 10) LOG("[d3d12] textura %08X fmt %u %ux%u: mip minimo %u de %u (streaming)", base, fmt, w, h, minLvl, lv);
        }
    }
    sd.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(map(swz & 7), map((swz >> 3) & 7), map((swz >> 6) & 7), map((swz >> 9) & 7));
    g_dev->CreateShaderResourceView(e.res.Get(), &sd, slot);
    return true;
}
} // namespace

// MSAA de RB_SURFACE_INFO (bits 16-17: 0 = 1x, 1 = 2x, 2 = 4x). En la EDRAM las muestras ocupan sitio:
// 2x = 1x2 (alto doble), 4x = 2x2. Se emula con una superficie de muestras (sesion 21), igual que las celdas de la EDRAM real,
// de modo que un clear a 640 con 4x cae en las mismas celdas que la escena a 1280 sin MSAA.
void MsaaScale(uint32_t& sx, uint32_t& sy)
{
    uint32_t m = (Reg(0x2000) >> 16) & 3;
    static bool noMsaa = getenv("GOWJ_NOMSAA") != nullptr;
    sx = (!noMsaa && m >= 2) ? 2 : 1; sy = (!noMsaa && m >= 1) ? 2 : 1;
}
// ---- texturas 3D (sesion 21): tablas de correccion de color y similares ----
// Direccion tiled 3D de Xenos (texture_address.h de Xenia): macro-tiles de 32x16x4, bancos alternos por (y/8) ^ (z/4).
static int64_t Tiled3DOffset(int32_t x, int32_t y, int32_t z, uint32_t pitchAligned, uint32_t heightAligned, uint32_t log2b)
{
    int32_t outer = ((((z >> 2) * int32_t(heightAligned >> 4) + (y >> 4)) * int32_t(pitchAligned >> 5)) + (x >> 5)) << 7;
    int32_t inner = ((z & 3) << 5) | (((y >> 1) & 3) << 3) | (x & 7);
    int64_t oi = int64_t(outer | inner) << log2b;
    uint32_t bank = ((y >> 3) ^ (z >> 2)) & 1, pipe = ((x >> 3) & 3) ^ (bank << 1);
    return int64_t((uint32_t(y & 1) << 4) | (pipe << 6) | (bank << 11)) | (oi & 0xF) | (((oi >> 4) & 1) << 5) | (((oi >> 5) & 7) << 8) | ((oi >> 8) << 12);
}
std::unordered_map<uint64_t, TexEntry> g_textures3D;
// GoWJ sesion 8 (rendimiento): la LUT de color (array 2D apilado 16x16x16 8888 leido con tfetch3D) se genera cada cuadro con un
// resolve por GPU de 16x512 (capa z en las filas 32z..32z+15). Antes se volcaba a memoria (esperando a la GPU a mitad de cuadro) para
// subirla como 3D; ahora se copia capa a capa en la GPU. Devuelve el resolve de origen si se puede. GOWJ_NOGPULUT=1 = antes.
struct GpuLut { ComPtr<ID3D12Resource> res; uint64_t gen = 0; D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_COPY_DEST; UINT w = 0, h = 0, d = 0; };
static std::unordered_map<uint64_t, GpuLut> g_gpuLuts;
static ResolvedTex* GpuLutSource(uint32_t tf)
{
    static const bool off = getenv("GOWJ_NOGPULUT") != nullptr;
    if (off) return nullptr;
    uint32_t fc[6]; for (int i = 0; i < 6; i++) fc[i] = Reg(0x4800 + tf * 6 + i);
    if ((fc[0] & 3) != 2 || ((fc[5] >> 9) & 3) != 1 || ((fc[2] >> 26) & 0x3F) == 0 || !(fc[0] >> 31) || (fc[1] & 0x3F) != 6) return nullptr;
    uint32_t base = fc[1] & 0xFFFFF000, endian = (fc[1] >> 6) & 3;
    uint32_t w = (fc[2] & 0x1FFF) + 1, h = ((fc[2] >> 13) & 0x1FFF) + 1, dpt = ((fc[2] >> 26) & 0x3F) + 1;
    uint32_t pitch = std::max<uint32_t>(((fc[0] >> 22) & 0x1FF) * 32, w), pitchAligned = (pitch + 31) & ~31u, hA = (h + 31) & ~31u;
    auto last = g_resolvedLast.find(base);
    if (last == g_resolvedLast.end()) return nullptr;
    auto it = g_resolved.find(last->second);
    if (it == g_resolved.end()) return nullptr;
    ResolvedTex& r = it->second;
    if (!r.res || r.texFmt != 6 || r.fmt != DXGI_FORMAT_B8G8R8A8_UNORM || r.sx != 1 || r.sy != 1 || r.endian != endian || w > r.w || (dpt - 1) * hA + h > r.h ||
        ((r.destPitch + 31) & ~31u) != pitchAligned) return nullptr;
    if (!ResolveMemOk(r, base)) return nullptr;   // la CPU reescribio la memoria
    return &r;
}
bool BindTexture3D(uint32_t tf, D3D12_CPU_DESCRIPTOR_HANDLE slot)
{
    uint32_t fc[6]; for (int i = 0; i < 6; i++) fc[i] = Reg(0x4800 + tf * 6 + i);
    if ((fc[0] & 3) != 2) return false;
    uint32_t dim = (fc[5] >> 9) & 3;
    // GoWJ sesion 3: array 2D apilado (dim 2D con profundidad en los bits 26-31) leido con tfetch3D: la LUT de color de UE3
    // (16x16x16 8888). Cada capa es una 2D tileada; se sube como textura 3D. GOWJ_NOSTACKED3D=1 = antes (LUT nula -> imagen negra).
    static const bool noStacked = getenv("GOWJ_NOSTACKED3D") != nullptr;
    bool stacked = dim == 1 && !noStacked && ((fc[2] >> 26) & 0x3F) > 0;
    if (dim != 2 && !stacked) return false;
    uint32_t fmt = fc[1] & 0x3F, endian = (fc[1] >> 6) & 3, base = fc[1] & 0xFFFFF000;
    // dword2 de una textura 3D: ancho 11 bits, alto 11 bits, profundidad 10 bits (menos uno)
    uint32_t w = (fc[2] & 0x7FF) + 1, h = ((fc[2] >> 11) & 0x7FF) + 1, dpt = ((fc[2] >> 22) & 0x3FF) + 1;
    if (stacked) { w = (fc[2] & 0x1FFF) + 1; h = ((fc[2] >> 13) & 0x1FFF) + 1; dpt = ((fc[2] >> 26) & 0x3F) + 1; }
    if (stacked)
        if (ResolvedTex* r = GpuLutSource(tf))
        {
            GpuLut& g = g_gpuLuts[(uint64_t(base) << 20) ^ (uint64_t(w) << 10) ^ (uint64_t(h) << 4) ^ dpt];
            if (!g.res || g.w != w || g.h != h || g.d != dpt)
            {
                if (g.res) g_deferredRelease[g_fi].push_back(g.res);
                g = GpuLut{};
                D3D12_HEAP_PROPERTIES def{ D3D12_HEAP_TYPE_DEFAULT };
                D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D; td.Width = w; td.Height = h; td.DepthOrArraySize = (UINT16)dpt;
                td.MipLevels = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
                if (FAILED(g_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g.res)))) { g.res.Reset(); goto lutMem; }
                g.w = w; g.h = h; g.d = dpt; g.st = D3D12_RESOURCE_STATE_COPY_DEST;
                LOG("[d3d12] LUT 3D %ux%ux%u en %08X generada en la GPU desde el resolve", w, h, dpt, base);
            }
            if (g.gen != r->gen)
            {
                D3D12_RESOURCE_BARRIER bs[2]; UINT nb = 0;
                if (r->state != D3D12_RESOURCE_STATE_COPY_SOURCE) { bs[nb++] = Transition(r->res.Get(), r->state, D3D12_RESOURCE_STATE_COPY_SOURCE); r->state = D3D12_RESOURCE_STATE_COPY_SOURCE; }
                if (g.st != D3D12_RESOURCE_STATE_COPY_DEST) { bs[nb++] = Transition(g.res.Get(), g.st, D3D12_RESOURCE_STATE_COPY_DEST); g.st = D3D12_RESOURCE_STATE_COPY_DEST; }
                if (nb) ProbedBarrier(nb, bs, __LINE__);
                const uint32_t hA = (h + 31) & ~31u;
                D3D12_TEXTURE_COPY_LOCATION dl{ g.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; dl.SubresourceIndex = 0;
                D3D12_TEXTURE_COPY_LOCATION sl{ r->res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; sl.SubresourceIndex = 0;
                for (uint32_t z = 0; z < dpt; z++) { D3D12_BOX box{ 0, z * hA, 0, w, z * hA + h, 1 }; g_list->CopyTextureRegion(&dl, 0, 0, z, &sl, &box); }
                auto b = Transition(g.res.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); ProbedBarrier(1, &b, __LINE__);
                g.st = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; g.gen = r->gen;
            }
            // Como el volcado + subida: sin intercambio, el texel B8G8R8A8 da (R,G,B,A); con swapRB se intercambian R y B
            const bool xs = r->swapRB; uint32_t swz = (fc[3] >> 1) & 0xFFF;
            auto map = [fmt, xs](uint32_t c) -> UINT { if (c < 4) { UINT hh = HostRep(fmt, c); return xs && (hh == 0 || hh == 2) ? 2 - hh : hh; } return c == 4 ? D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0 : D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1; };
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D; sd.Texture3D.MipLevels = 1;
            sd.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(map(swz & 7), map((swz >> 3) & 7), map((swz >> 6) & 7), map((swz >> 9) & 7));
            g_dev->CreateShaderResourceView(g.res.Get(), &sd, slot);
            return true;
        }
lutMem:
    if (!g_pendingResolvesEmpty()) FlushResolvesIfOverlap(base, base + std::min<uint32_t>(std::max(w, 32u) * std::max(h, 32u) * std::max(dpt, 4u) * 16u, 64u << 20));
    bool tiled = fc[0] >> 31; uint32_t pitch = std::max<uint32_t>(((fc[0] >> 22) & 0x1FF) * 32, w);
    DXGI_FORMAT df; uint32_t bpp;
    switch (fmt)
    {
    case 6: case 14: df = DXGI_FORMAT_R8G8B8A8_UNORM; bpp = 4; break;
    case 7: df = DXGI_FORMAT_R10G10B10A2_UNORM; bpp = 4; break;
    case 2: df = DXGI_FORMAT_R8_UNORM; bpp = 1; break;
    default:
    {
        static std::unordered_map<uint32_t, int> seen;
        if (seen[fmt]++ == 0) LOG("[d3d12] textura 3D con formato %u sin soporte (%ux%ux%u)", fmt, w, h, dpt);
        return false;
    }
    }
    if (!base || w > 2048 || h > 2048 || dpt > 1024) return false;
    uint32_t log2b = bpp == 1 ? 0 : 2;
    const uint8_t* src = g_base + (0xA0000000u | (base & 0x1FFFFFFF));
    uint64_t dh = 1469598103934665603ull;
    for (uint32_t i = 0; i < 64; i++) { dh ^= ((const uint32_t*)src)[i * 37 % 4096]; dh *= 1099511628211ull; }
    uint64_t key = TexKey(base, fmt, w, h, dpt | (stacked ? 0x80000000u : 0), 3);
    TexEntry& e = g_textures3D[key];
    if (!e.res || e.dataHash != dh)
    {
        if (!e.res)
        {
            D3D12_HEAP_PROPERTIES def{ D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D; td.Width = w; td.Height = h; td.DepthOrArraySize = (UINT16)dpt;
            td.MipLevels = 1; td.Format = df; td.SampleDesc.Count = 1;
            if (FAILED(CreateTextureResource(td, e.res))) return false;
            e.fmt = df;
            LOG("[d3d12] textura 3D %ux%ux%u formato %u %s%s en %08X", w, h, dpt, fmt, tiled ? "tiled" : "lineal", stacked ? " (array 2D apilado)" : "", base);
        }
        else { auto b = Transition(e.res.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST); ProbedBarrier(1, &b, __LINE__); }
        UINT rowPitch = (w * bpp + 255) & ~255u;
        UINT64 o = RingAlloc(UINT64(rowPitch) * h * dpt, 512);
        if (o == ~0ull) { auto bb = Transition(e.res.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); ProbedBarrier(1, &bb, __LINE__); return false; }   // anillo lleno: la textura vuelve a su estado (sesion 8)
        uint8_t* dst = g_ringMap[g_fi] + o;
        uint32_t pitchAligned = (pitch + 31) & ~31u, heightAligned = (h + 31) & ~31u;
        for (uint32_t z = 0; z < dpt; z++)
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++)
                {
                    uint64_t so = stacked ? uint64_t(z) * pitchAligned * heightAligned * bpp +
                                              (tiled ? (uint64_t)TiledInner(x, y, log2b, TiledOuter(y, pitchAligned, log2b)) : (uint64_t(y) * pitchAligned + x) * bpp)
                                : tiled ? (uint64_t)Tiled3DOffset(x, y, z, pitchAligned, heightAligned, log2b)
                                        : ((uint64_t(z) * heightAligned + y) * pitchAligned + x) * bpp;
                    uint8_t* dp = dst + (uint64_t(z) * h + y) * rowPitch + x * bpp;
                    if (bpp == 1) *dp = src[so];
                    else *(uint32_t*)dp = Swap32(*(const uint32_t*)(src + so), endian);
                }
        D3D12_TEXTURE_COPY_LOCATION d{ e.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; d.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION sl{ g_ring[g_fi].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        sl.PlacedFootprint.Offset = o; sl.PlacedFootprint.Footprint = { df, w, h, dpt, rowPitch };
        if (bpp == 4 && ((w >= 32 && h >= 32 && dpt >= 16) || stacked))
        {
            // Diagnostico (sesion 23): esquinas y centro de la tabla 3D (una tabla neutra va de 000000 a FFFFFF)
            static int logged = 0;
            if (logged++ < 6)
            {
                auto at = [&](uint32_t x, uint32_t y, uint32_t z) { return *(uint32_t*)(dst + (uint64_t(z) * h + y) * rowPitch + x * 4); };
                uint32_t m = w - 1, n = h - 1, q = dpt - 1;
                LOG("[d3d12] tabla 3D %08X: (0,0,0)=%08X (m,0,0)=%08X (0,m,0)=%08X (0,0,m)=%08X (m,m,m)=%08X centro=%08X", base,
                    at(0, 0, 0), at(m, 0, 0), at(0, n, 0), at(0, 0, q), at(m, n, q), at(w / 2, h / 2, dpt / 2));
            }
        }
        CmdTrace("tex3D %08X fmt %u %ux%ux%u rp %u off %llu", base, fmt, w, h, dpt, rowPitch, (unsigned long long)o);
        g_list->CopyTextureRegion(&d, 0, 0, 0, &sl, nullptr);
        auto b = Transition(e.res.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        ProbedBarrier(1, &b, __LINE__);
        e.dataHash = dh;
    }
    uint32_t swz = (fc[3] >> 1) & 0xFFF;
    auto map = [fmt](uint32_t c) -> UINT { return c < 4 ? HostRep(fmt, c) : (c == 4 ? D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0 : D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1); };
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = e.fmt; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D; sd.Texture3D.MipLevels = 1;
    sd.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(map(swz & 7), map((swz >> 3) & 7), map((swz >> 6) & 7), map((swz >> 9) & 7));
    g_dev->CreateShaderResourceView(e.res.Get(), &sd, slot);
    return true;
}

// ---- texturas cubo (sesion 21): 6 caras 2D consecutivas, cada una alineada a 32 bloques de alto y a 4 KB ----
std::unordered_map<uint64_t, TexEntry> g_texturesCube;
bool BindTextureCube(uint32_t tf, D3D12_CPU_DESCRIPTOR_HANDLE slot)
{
    static bool noCube = getenv("GOWJ_NOCUBE") != nullptr;   // diagnostico
    if (noCube) return false;
    uint32_t fc[6]; for (int i = 0; i < 6; i++) fc[i] = Reg(0x4800 + tf * 6 + i);
    if ((fc[0] & 3) != 2 || ((fc[5] >> 9) & 3) != 3) return false;
    uint32_t fmt = fc[1] & 0x3F, endian = (fc[1] >> 6) & 3, base = fc[1] & 0xFFFFF000;
    uint32_t w = (fc[2] & 0x1FFF) + 1, h = ((fc[2] >> 13) & 0x1FFF) + 1;
    PruneStaleResolve(base);   // GoWJ sesion 11
    if (!g_pendingResolvesEmpty())
    {   // resolves por CPU pendientes que escriben en esta textura (o sus mips): completarlos antes de leer la memoria
        uint32_t span = std::min<uint32_t>(std::max(w, 32u) * std::max(h, 32u) * 16u, 64u << 20);
        FlushResolvesIfOverlap(base, base + span);
        if (uint32_t ma = fc[5] & 0xFFFFF000) FlushResolvesIfOverlap(ma, ma + span);
    }
    bool tiled = fc[0] >> 31; uint32_t pitchTexels = ((fc[0] >> 22) & 0x1FF) * 32;
    DXGI_FORMAT df; uint32_t blockBytes, blockDim;
    switch (fmt)
    {
    case 6: case 14: case 50: df = DXGI_FORMAT_R8G8B8A8_UNORM; blockBytes = 4; blockDim = 1; break;   // 50 = 8_8_8_8_AS_16_16_16_16
    case 54: df = DXGI_FORMAT_R10G10B10A2_UNORM; blockBytes = 4; blockDim = 1; break;   // GoWJ: 2_10_10_10_AS_16_16_16_16 (mismos bits que 7)
    case 51: df = DXGI_FORMAT_BC1_UNORM; blockBytes = 8; blockDim = 4; break;           // DXT1_AS_16_16_16_16
    case 52: df = DXGI_FORMAT_BC2_UNORM; blockBytes = 16; blockDim = 4; break;          // DXT2_3_AS_16_16_16_16
    case 53: df = DXGI_FORMAT_BC3_UNORM; blockBytes = 16; blockDim = 4; break;          // DXT4_5_AS_16_16_16_16
    case 18: df = DXGI_FORMAT_BC1_UNORM; blockBytes = 8; blockDim = 4; break;
    case 19: df = DXGI_FORMAT_BC2_UNORM; blockBytes = 16; blockDim = 4; break;
    case 20: df = DXGI_FORMAT_BC3_UNORM; blockBytes = 16; blockDim = 4; break;
    case 32: df = DXGI_FORMAT_R16G16B16A16_FLOAT; blockBytes = 8; blockDim = 1; break;
    default:
    {
        static std::unordered_map<uint32_t, int> seen;
        if (seen[fmt]++ == 0) LOG("[d3d12] textura cubo con formato %u sin soporte (%ux%u)", fmt, w, h);
        return false;
    }
    }
    if (!base || w > 4096 || h > 4096) return false;
    uint32_t bw = (w + blockDim - 1) / blockDim, bh = (h + blockDim - 1) / blockDim;
    uint32_t pitchBlocks = std::max<uint32_t>((pitchTexels ? pitchTexels : w) / blockDim, bw);
    uint32_t log2b = blockBytes == 4 ? 2 : blockBytes == 8 ? 3 : 4;
    uint32_t pitchAligned = (pitchBlocks + 31) & ~31u, heightAligned = (bh + 31) & ~31u;
    uint64_t faceStride = (uint64_t(pitchAligned) * heightAligned * blockBytes + 0xFFF) & ~0xFFFull;
    const uint8_t* src = g_base + (0xA0000000u | (base & 0x1FFFFFFF));
    uint64_t dh = 1469598103934665603ull;
    for (uint32_t i = 0; i < 64; i++) { dh ^= ((const uint32_t*)src)[i * 37 % 4096]; dh *= 1099511628211ull; }
    uint64_t key = TexKey(base, fmt, w, h, 0, 6);
    TexEntry& e = g_texturesCube[key];
    if (!e.res || e.dataHash != dh)
    {
        if (!e.res)
        {
            D3D12_HEAP_PROPERTIES def{ D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = bw * blockDim; td.Height = bh * blockDim; td.DepthOrArraySize = 6;
            td.MipLevels = 1; td.Format = df; td.SampleDesc.Count = 1;
            if (FAILED(CreateTextureResource(td, e.res))) return false;
            e.fmt = df;
            LOG("[d3d12] textura cubo %ux%u formato %u %s en %08X", w, h, fmt, tiled ? "tiled" : "lineal", base);
        }
        else { auto b = Transition(e.res.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST); ProbedBarrier(1, &b, __LINE__); }
        UINT rowPitch = (bw * blockBytes + 255) & ~255u;
        UINT64 faceBytes = (UINT64(rowPitch) * bh + 511) & ~511ull;
        UINT64 o = RingAlloc(faceBytes * 6, 512);
        if (o == ~0ull) { auto bb = Transition(e.res.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); ProbedBarrier(1, &bb, __LINE__); return false; }   // anillo lleno: la textura vuelve a su estado (sesion 8)
        for (uint32_t face = 0; face < 6; face++)
        {
            const uint8_t* fsrc = src + face * faceStride;
            uint8_t* dst = g_ringMap[g_fi] + o + face * faceBytes;
            for (uint32_t by = 0; by < bh; by++)
            {
                uint32_t outer = tiled ? TiledOuter(by, pitchAligned, log2b) : 0;
                for (uint32_t bx = 0; bx < bw; bx++)
                {
                    uint32_t so = tiled ? TiledInner(bx, by, log2b, outer) : (by * pitchBlocks + bx) * blockBytes;
                    const uint32_t* sp = (const uint32_t*)(fsrc + so);
                    uint32_t* dp = (uint32_t*)(dst + by * rowPitch + bx * blockBytes);
                    for (uint32_t k = 0; k < blockBytes / 4; k++) dp[k] = Swap32(sp[k], endian);
                }
            }
            D3D12_TEXTURE_COPY_LOCATION d{ e.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; d.SubresourceIndex = face;
            D3D12_TEXTURE_COPY_LOCATION sl{ g_ring[g_fi].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
            if (face == 0) CmdTrace("texCubo %08X fmt %u %ux%u bloques %ux%u rp %u off %llu", base, fmt, w, h, bw, bh, rowPitch, (unsigned long long)o);
            sl.PlacedFootprint.Offset = o + face * faceBytes; sl.PlacedFootprint.Footprint = { df, bw * blockDim, bh * blockDim, 1, rowPitch };
            g_list->CopyTextureRegion(&d, 0, 0, 0, &sl, nullptr);
        }
        auto b = Transition(e.res.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        ProbedBarrier(1, &b, __LINE__);
        e.dataHash = dh;
    }
    uint32_t swz = (fc[3] >> 1) & 0xFFF;
    auto map = [fmt](uint32_t c) -> UINT { return c < 4 ? HostRep(fmt, c) : (c == 4 ? D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0 : D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1); };
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = e.fmt; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE; sd.TextureCube.MipLevels = 1;
    sd.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(map(swz & 7), map((swz >> 3) & 7), map((swz >> 6) & 7), map((swz >> 9) & 7));
    g_dev->CreateShaderResourceView(e.res.Get(), &sd, slot);
    return true;
}

EdramRt* GetEdramRt(uint32_t infoReg = 0x2001)   // RB_COLOR_INFO; MRT: RB_COLOR1_INFO 0x2003, 2 = 0x2004, 3 = 0x2005 (sesion 24)
{
    uint32_t base = Reg(infoReg) & 0xFFF, pitch = Reg(0x2000) & 0x3FFF, xfmt = (Reg(infoReg) >> 16) & 0xF;
    if (!pitch) pitch = kRtW;
    {   // diagnostico (sesion 24): sesgo de exponente del color (bits 20-25)
        static std::unordered_map<uint32_t, int> seenBias;
        uint32_t ci = Reg(infoReg);
        if ((ci >> 20) & 0x3F && seenBias.size() < 32 && seenBias[ci]++ == 0) LOG("[edram] RB_COLOR_INFO %08X con sesgo de exponente %d (pitch %u)", ci, (int)(((ci >> 20) & 0x3F) ^ 0x20) - 0x20, pitch);
    }
    const uint32_t wideS = SurfS(pitch), tallS = SurfSy(pitch);
    { uint32_t sx, sy; MsaaScale(sx, sy); pitch *= sx; }
    static bool only8888 = getenv("GOWJ_RT8888") != nullptr;   // diagnostico: comportamiento anterior
    if (only8888) xfmt = 0;
    // Sesion 25: 2_10_10_10_FLOAT_AS_16_16_16_16 (12) y 2_10_10_10_AS_10_10_10_10 (10) son los mismos datos de EDRAM que 3 y 2
    // (solo cambia la precision de la mezcla). El juego dibuja la escena en formato 3 y luego los efectos (sangre, chispas, agua...)
    // en formato 12 sobre la misma base: con superficies separadas los efectos nunca llegaban a la pantalla. GOWJ_SPLITAS16=1 = antes.
    static const bool splitAs16 = getenv("GOWJ_SPLITAS16") != nullptr;
    if (!splitAs16) { if (xfmt == 12) xfmt = 3; else if (xfmt == 10) xfmt = 2; }
    DXGI_FORMAT f; UINT bpp; float scale;
    EdramFormat(xfmt, f, bpp, scale);
    if (f == DXGI_FORMAT_B8G8R8A8_UNORM) xfmt = 0;   // 8888 y 8888_GAMMA comparten superficie
    uint32_t key = (xfmt << 28) | (base << 14) | pitch;
    auto found = g_edramRts.find(key);
    if (found != g_edramRts.end()) return &found->second;
    if (g_edramRts.size() >= 60) return nullptr;
    EdramRt& rt = g_edramRts[key];
    rt.w = pitch * wideS; rt.sx = wideS; rt.sy = tallS; rt.h = 2 * kEdramH * tallS; rt.rtv = kFrames + 1 + (UINT)g_edramRts.size(); rt.base = base;
    rt.xfmt = xfmt; rt.fmt = f; rt.bpp = bpp; rt.outScale = scale;
    D3D12_HEAP_PROPERTIES def{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = rt.w; td.Height = rt.h; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = f; td.SampleDesc.Count = 1; td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv{ f, { 0, 0, 0, 1 } };
    if (FAILED(g_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&rt.res)))) { g_edramRts.erase(key); return nullptr; }
    g_dev->CreateRenderTargetView(rt.res.Get(), nullptr, Rtv(rt.rtv));
    const float black[4] = { 0, 0, 0, 1 };
    g_list->ClearRenderTargetView(Rtv(rt.rtv), black, 0, nullptr);
    LOG("[edram] nueva superficie: base %u tiles, pitch %u, formato %u (DXGI %d), %ux%u", base, pitch, xfmt, (int)f, rt.w, rt.h);
    return &rt;
}

D3D12_CPU_DESCRIPTOR_HANDLE Dsv(UINT i) { auto h = g_dsvHeap->GetCPUDescriptorHandleForHeapStart(); h.ptr += i * g_dsvSize; return h; }

// RB_DEPTH_INFO (0x2002): bits 0-11 base en tiles, bit 16 formato (0 = D24S8, 1 = D24FS8). El pitch es el de RB_SURFACE_INFO.
DepthRt* GetDepthRt()
{
    uint32_t base = Reg(0x2002) & 0xFFF, pitch = Reg(0x2000) & 0x3FFF;
    if (!pitch) pitch = kRtW;
    const uint32_t wideS = SurfS(pitch), tallS = SurfSy(pitch);
    { uint32_t sx, sy; MsaaScale(sx, sy); pitch *= sx; }
    uint32_t key = (base << 16) | pitch;
    auto found = g_depthRts.find(key);
    if (found != g_depthRts.end()) return &found->second;
    if (g_depthRts.size() >= 60) return nullptr;
    DepthRt& d = g_depthRts[key];
    d.w = pitch * wideS; d.sx = wideS; d.sy = tallS; d.h = 2 * kEdramH * tallS; d.dsv = (UINT)g_depthRts.size() - 1;
    D3D12_HEAP_PROPERTIES def{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = d.w; td.Height = d.h; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = kDepthFmt; td.SampleDesc.Count = 1; td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE cv{}; cv.Format = kDepthFmt; cv.DepthStencil = { 1.0f, 0 };
    if (FAILED(g_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_PPV_ARGS(&d.res)))) { g_depthRts.erase(key); return nullptr; }
    g_dev->CreateDepthStencilView(d.res.Get(), nullptr, Dsv(d.dsv));
    g_list->ClearDepthStencilView(Dsv(d.dsv), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
    LOG("[edram] nueva superficie de profundidad: base %u tiles, pitch %u, formato %s", base, pitch, (Reg(0x2002) >> 16) & 1 ? "D24FS8" : "D24S8");
    return &d;
}

// Valor de RB_DEPTH_CLEAR (depth en los 24 bits altos, stencil en los 8 bajos) -> float
float DecodeDepth24(uint32_t v24, bool isFloat)
{
    if (!isFloat) return v24 / 16777215.0f;
    // 20e4: 4 bits de exponente (sesgo 15), 20 de mantisa
    // Sesion 23 (rendimiento): con bits en vez de ldexpf (las texturas de profundidad de 1024x1024 tardaban 37 ms en subirse).
    // Exacto: exponente + 112 (127 - 15) y la mantisa de 20 bits en los 20 altos de la de 23; los desnormales, m * 2^-34.
    uint32_t e = v24 >> 20, m = v24 & 0xFFFFF;
    if (!e) return (float)m * 5.820766091346741e-11f;   // 2^-34
    uint32_t bits = ((e + 112u) << 23) | (m << 3); float f; memcpy(&f, &bits, 4); return f;
}

D3D12_COMPARISON_FUNC MapCmp(uint32_t f)
{
    static const D3D12_COMPARISON_FUNC t[8] = { D3D12_COMPARISON_FUNC_NEVER, D3D12_COMPARISON_FUNC_LESS, D3D12_COMPARISON_FUNC_EQUAL, D3D12_COMPARISON_FUNC_LESS_EQUAL,
        D3D12_COMPARISON_FUNC_GREATER, D3D12_COMPARISON_FUNC_NOT_EQUAL, D3D12_COMPARISON_FUNC_GREATER_EQUAL, D3D12_COMPARISON_FUNC_ALWAYS };
    return t[f & 7];
}
D3D12_STENCIL_OP MapStencilOp(uint32_t o)
{
    static const D3D12_STENCIL_OP t[8] = { D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_ZERO, D3D12_STENCIL_OP_REPLACE, D3D12_STENCIL_OP_INCR_SAT,
        D3D12_STENCIL_OP_DECR_SAT, D3D12_STENCIL_OP_INVERT, D3D12_STENCIL_OP_INCR, D3D12_STENCIL_OP_DECR };
    return t[o & 7];
}

// Con GOWJ_D3D12_DEBUG=1: vuelca al log los mensajes de la capa de depuracion (errores y avisos)
void DrainDebugMessages()
{
    static ComPtr<ID3D12InfoQueue> q;
    static bool tried = false;
    if (!tried) { tried = true; if (getenv("GOWJ_D3D12_DEBUG")) g_dev.As(&q); }
    if (!q) return;
    static int total = 0;
    UINT64 n = q->GetNumStoredMessages();
    for (UINT64 i = 0; i < n && total < 60; i++)
    {
        SIZE_T len = 0; q->GetMessage(i, nullptr, &len);
        std::vector<char> buf(len);
        auto* m = (D3D12_MESSAGE*)buf.data();
        if (SUCCEEDED(q->GetMessage(i, m, &len)) && m->Severity <= (getenv("GOWJ_D3D12_WARN") ? D3D12_MESSAGE_SEVERITY_WARNING : D3D12_MESSAGE_SEVERITY_ERROR)) { total++; LOG("[d3d12 debug] %.400s", m->pDescription); }
    }
    q->ClearStoredMessages();
}

// Traza de operaciones que graban comandos especiales (sesion 23): si Close falla, se vuelca para saber cual fue
std::vector<std::string> g_cmdTrace;
std::string g_pendingOp;
std::string g_swapResolves;   // destinos de resolve desde el ultimo swap (GOWJ_SWAPLOG)
int g_closeFailures = 0;   // con GOWJ_FLUSHEACH: operacion que se iba a registrar cuando fallo el Close (las subidas se graban antes de su traza)
void FlushAndWait();
bool g_flushAfterResolve = false;   // cerrar la lista al empezar el siguiente dibujo (tras un resolve con MSAA)
static const DWORD flushT0 = GetTickCount();   // arranque del proceso
void CmdTrace(const char* fmt, ...)
{
    // GOWJ_FLUSHEACH=1: cierra y ejecuta la lista antes de cada operacion, para que un fallo de Close senale la anterior
    // GOWJ_FLUSHEACH=t<segundos>: solo a partir de ese momento del arranque (sesion 8)
    static const char* flushS = getenv("GOWJ_FLUSHEACH");
    static const DWORD flushAfter = (flushS && flushS[0] == 't') ? (DWORD)(atof(flushS + 1) * 1000) : 0;
    bool flushEach = flushS && GetTickCount() - flushT0 >= flushAfter;
    // GoWJ sesion 8 (rendimiento): el rastro (vsnprintf + std::string por draw) solo con GOWJ_CMDTRACE=1 o los modos de vaciado
    static const char* flushResS0 = getenv("GOWJ_FLUSHRESOLVE");
    static const bool traceOn = getenv("GOWJ_CMDTRACE") != nullptr || flushS || (flushResS0 && flushResS0[0] != '0');
    if (!traceOn) return;
    char b[256]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    // GOWJ_FLUSHRESOLVE=1: cierra la lista antes y despues de cada resolve (aisla el resolve que la invalida, sesion 8)
    // Por defecto solo los resolves con MSAA: con ellos en la misma lista que los dibujos anteriores, Close falla (E_INVALIDARG)
    // y sin capa de depuracion la GPU se cuelga en la escena 3D (causa exacta sin aislar). GOWJ_FLUSHRESOLVE=0 lo desactiva.
    static const char* flushResS = getenv("GOWJ_FLUSHRESOLVE");
    static const bool flushRes = flushResS && flushResS[0] != '0';   // diagnostico (no evita el cuelgue)
    static const bool flushAllRes = flushResS && flushResS[0] == '1';
    static bool prevResolve = false;
    bool isRes = strncmp(b, "resolve", 7) == 0 && (flushAllRes || !strstr(b, "msaa 1x1"));
    if (flushRes && g_frameOpen && isRes) { g_pendingOp = b; FlushAndWait(); g_pendingOp.clear(); g_flushAfterResolve = true; }
    (void)prevResolve;
    // El cierre se hace al principio del siguiente dibujo (punto seguro); cerrar aqui, a mitad de un dibujo, tumbaba el driver
    if (flushEach && g_frameOpen) g_flushAfterResolve = true;
    if (g_cmdTrace.size() >= 400) g_cmdTrace.erase(g_cmdTrace.begin(), g_cmdTrace.begin() + 100);   // se guardan las ultimas (sesion 8)
    g_cmdTrace.emplace_back(b);
}
void DumpCmdTrace(HRESULT hr)
{
    static int dumps = 0;
    g_closeFailures++;
    // Tras un Close fallido la lista queda en error para siempre (los siguientes Close devuelven E_FAIL aunque se reinicie):
    // se crea una lista nueva y se cierra, para que el codigo siguiente la reinicie con normalidad (sesion 23).
    {
        ComPtr<ID3D12GraphicsCommandList> fresh;
        if (SUCCEEDED(g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[g_fi].Get(), nullptr, IID_PPV_ARGS(&fresh))))
        {
            fresh->Close();
            g_list = fresh;
        }
    }
    if (dumps++ < 8)
    {
        LOG("[d3d12] Close fallo (0x%08lX): ultimas operaciones de la lista (%zu):", (unsigned long)hr, g_cmdTrace.size());
        size_t from = (dumps == 1 || g_cmdTrace.size() <= 40) ? 0 : g_cmdTrace.size() - 40;   // el primer fallo, entero
        for (size_t i = from; i < g_cmdTrace.size(); i++) LOG("[d3d12]   %s", g_cmdTrace[i].c_str());
        if (!g_pendingOp.empty()) LOG("[d3d12]   (siguiente: %s)", g_pendingOp.c_str());
    }
}

void FlushAndWait()
{
    struct FlushTimer { double t0 = QpcMs(); ~FlushTimer() { g_hFlushMs += QpcMs() - t0; g_hFlushes++; } } ftimer;
    {   // GOWJ_FLUSHSTATS=1: de donde se llama (direccion de retorno, simbolizable con llvm-symbolizer) (sesion 24)
        static const bool on = getenv("GOWJ_FLUSHSTATS") != nullptr;
        if (on)
        {
            static std::unordered_map<uintptr_t, uint64_t> sites; static uint64_t total = 0;
            uintptr_t ra = (uintptr_t)__builtin_return_address(0) - (uintptr_t)GetModuleHandleW(nullptr) + 0x140000000ull;
            sites[ra]++;
            if (++total % 500 == 0) { std::string l; for (auto& kv : sites) { char b[64]; snprintf(b, sizeof(b), " %llX=%llu", (unsigned long long)kv.first, (unsigned long long)kv.second); l += b; } LOG("[flushstats]%s", l.c_str()); }
        }
    }
    DrainDebugMessages();
    HRESULT chr = g_list->Close();
    if (FAILED(chr))
    {
        // Lista invalida: se descarta (ejecutarla haria perder el dispositivo para siempre)
        static int n = 0; if (n++ < 10) LOG("[d3d12] Close de la lista fallo (0x%08lX, draws en el frame %llu, t=%.1f s): se descarta", (unsigned long)chr, (unsigned long long)g_frameDraws, (GetTickCount() - flushT0) / 1000.0);
        DumpCmdTrace(chr);
    }
    else
    {
        ID3D12CommandList* lists[] = { g_list.Get() };
        g_queue->ExecuteCommandLists(1, lists);
    }
    g_cmdTrace.clear();
    g_queue->Signal(g_fence.Get(), g_nextFence);
    g_fence->SetEventOnCompletion(g_nextFence++, g_fenceEvent);
    WaitForSingleObject(g_fenceEvent, 5000);
    g_alloc[g_fi]->Reset();
    g_list->Reset(g_alloc[g_fi].Get(), nullptr);
    g_boundRt = nullptr;
}

// ---- conversion de pixeles en los resolves de superficies que no son 8888 (sesion 23, segun resolve.xesli de Xenia) ----
static float HalfToFloat(uint16_t h)
{
    // Sesion 16 de NG2 (rendimiento): con bits en vez de ldexpf (era el 25 % del hilo de la GPU en la escena 3D)
    uint32_t s = uint32_t(h & 0x8000) << 16, e = (h >> 10) & 0x1F, m = h & 0x3FF, bits;
    if (e == 0) { float f = (float)m * (1.0f / 16777216.0f); return s ? -f : f; }   // cero o desnormal
    bits = e == 31 ? (s | 0x7F800000u | (m << 13)) : (s | ((e + 112) << 23) | (m << 13));
    float f; memcpy(&f, &bits, 4); return f;
}
// Pixel de nuestro render target -> 4 floats con el rango del formato de Xenos
static void UnpackRtPixel(DXGI_FORMAT f, const uint8_t* px, float v[4])
{
    v[0] = v[1] = v[2] = 0; v[3] = 1;
    switch (f)
    {
    case DXGI_FORMAT_R16G16_SNORM:            // 16_16 de Xenos: -32..32 guardado como SNORM x 1/32
        for (int k = 0; k < 2; k++) v[k] = std::max(-1.0f, ((const int16_t*)px)[k] / 32767.0f) * 32.0f; v[2] = 0; v[3] = 0; break;
    case DXGI_FORMAT_R16G16B16A16_SNORM:      // 16_16_16_16
        for (int k = 0; k < 4; k++) v[k] = std::max(-1.0f, ((const int16_t*)px)[k] / 32767.0f) * 32.0f; break;
    case DXGI_FORMAT_R16G16_FLOAT:
        v[0] = HalfToFloat(((const uint16_t*)px)[0]); v[1] = HalfToFloat(((const uint16_t*)px)[1]); v[2] = 0; v[3] = 0; break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        for (int k = 0; k < 4; k++) v[k] = HalfToFloat(((const uint16_t*)px)[k]); break;
    case DXGI_FORMAT_R32_FLOAT: memcpy(&v[0], px, 4); v[1] = v[2] = 0; v[3] = 0; break;
    case DXGI_FORMAT_R32G32_FLOAT: memcpy(v, px, 8); v[2] = 0; v[3] = 0; break;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    {
        uint32_t w = *(const uint32_t*)px;
        v[0] = (w & 1023) / 1023.0f; v[1] = ((w >> 10) & 1023) / 1023.0f; v[2] = ((w >> 20) & 1023) / 1023.0f; v[3] = (w >> 30) / 3.0f; break;
    }
    default: break;
    }
}
// Float de 32 bits -> float7e3 de Xenos (sin signo: 3 bits de exponente, 7 de mantisa, maximo 31,875), con redondeo al par
static uint32_t Float32To7e3(float f)
{
    if (!(f > 0.0f)) return 0;
    if (f >= 31.875f) return 0x3FF;
    uint32_t u; memcpy(&u, &f, 4);
    if (u < 0x3E800000u) { uint32_t shift = 125 - (u >> 23); u = shift >= 24 ? 0 : (0x800000u | (u & 0x7FFFFFu)) >> shift; }   // desnormal (< 0,25)
    else u += 0xC2000000u;   // exponente -124
    return ((u + 0x7FFFu + ((u >> 16) & 1u)) >> 16) & 0x3FFu;
}
static bool PackResolveSupported(uint32_t fmt)
{
    switch (fmt) { case 6: case 7: case 25: case 26: case 31: case 32: case 36: case 37: return true; default: return false; }
}
// 4 floats -> formato de destino del resolve (palabras de 32 bits en orden de memoria antes del endian)
static void PackResolvePixel(uint32_t fmt, const float v[4], uint32_t w[2])
{
    auto un = [](float x, float mx) { return (uint32_t)(std::clamp(std::isnan(x) ? 0.0f : x, 0.0f, 1.0f) * mx + 0.5f); };
    auto fb = [](float x) { uint32_t u; memcpy(&u, &x, 4); return u; };
    switch (fmt)
    {
    case 6:  w[0] = un(v[0], 255) | (un(v[1], 255) << 8) | (un(v[2], 255) << 16) | (un(v[3], 255) << 24); break;            // 8_8_8_8
    case 7:  w[0] = un(v[0], 1023) | (un(v[1], 1023) << 10) | (un(v[2], 1023) << 20) | (un(v[3], 3) << 30); break;        // 2_10_10_10
    case 25: w[0] = un(v[0], 65535) | (un(v[1], 65535) << 16); break;                                                    // 16_16 (sin signo)
    case 26: w[0] = un(v[0], 65535) | (un(v[1], 65535) << 16); w[1] = un(v[2], 65535) | (un(v[3], 65535) << 16); break;  // 16_16_16_16
    case 31: w[0] = FloatToHalf(v[0]) | (uint32_t(FloatToHalf(v[1])) << 16); break;                                     // 16_16_FLOAT
    case 32: w[0] = FloatToHalf(v[0]) | (uint32_t(FloatToHalf(v[1])) << 16); w[1] = FloatToHalf(v[2]) | (uint32_t(FloatToHalf(v[3])) << 16); break;
    case 36: w[0] = fb(v[0]); break;                                                                                     // 32_FLOAT
    case 37: w[0] = fb(v[0]); w[1] = fb(v[1]); break;                                                                    // 32_32_FLOAT
    default: break;
    }
}

// Resolve: copia un rectangulo de la superficie EDRAM actual a memoria fisica (tiled), y clear opcional.
static long long DrawLogFrame();
// Sesion 24 (rendimiento): resolves por CPU diferidos. Antes cada uno hacia FlushAndWait (esperar a que la GPU acabe todo)
// y convertia los pixeles al momento: ~33 % del hilo de comandos en canciones. Ahora la copia a memoria de lectura se graba
// en la lista y la conversion se aplaza hasta que alguien necesita esa memoria: una textura o vertices que la solapan, un
// paquete que la CPU del juego puede observar (escrituras a memoria, eventos, interrupciones, swap) o el final del lote.
// Se espera una sola vez por lote. Experimental y desactivado: GOWJ_DEFERRESOLVE=1 (se cerraba al cargar escenarios).
namespace {
struct PendingResolve { UINT64 off; uint32_t lo, hi; std::function<void(const uint8_t*)> fn; uint64_t sig = 0; uint64_t frame = 0; };
uint64_t DeferSig(uint32_t lo, uint32_t hi) { uint64_t h = 1469598103934665603ull; for (uint32_t a = lo; a + 4 <= hi; a += 4093 & ~3u) { h ^= *(const uint32_t*)(g_base + (0xA0000000u | a)); h *= 1099511628211ull; } return h; }
std::vector<PendingResolve> g_pendingResolves;
uint64_t g_flushTotal = 0, g_flushOverlap = 0, g_deferredCount = 0;
ComPtr<ID3D12Resource> g_readbackDefer;
UINT64 g_readbackDeferOff = 0;
constexpr UINT64 kReadbackDeferSize = 64ull << 20;
void FlushPendingResolves()
{
    if (g_pendingResolves.empty()) return;
    g_flushTotal++; g_deferredCount += g_pendingResolves.size();
    FlushAndWait();
    uint8_t* p = nullptr; g_readbackDefer->Map(0, nullptr, (void**)&p);
    for (auto& pr : g_pendingResolves)
    {
        static int logged = 0;
        if (logged < 40 && DeferSig(pr.lo, pr.hi) != pr.sig) { logged++; LOG("[defer] la memoria %08X-%08X cambio antes de completar el resolve (frame %llu -> %llu)", pr.lo, pr.hi, (unsigned long long)pr.frame, (unsigned long long)g_frameCounter); }
    }
    if (p) for (auto& pr : g_pendingResolves) pr.fn(p + pr.off);
    if (p) g_readbackDefer->Unmap(0, nullptr);
    g_pendingResolves.clear();
    g_readbackDeferOff = 0;
    g_cpuResolveCounter++;
}
bool g_pendingResolvesEmpty() { return g_pendingResolves.empty(); }
void FlushResolvesIfOverlap(uint32_t lo, uint32_t hi)
{
    uint32_t len = hi - lo; lo &= 0x1FFFFFFF; hi = lo + len;
    for (auto& pr : g_pendingResolves) if (pr.lo < hi && lo < pr.hi) { g_flushOverlap++; FlushPendingResolves(); return; }
}

} // namespace

// Sesion 19 de NG2: bufer de lectura comun (resolve por CPU de color y profundidad, volcados). Antes solo lo creaba el resolve
// de color por CPU: con los resolves float por GPU nunca se creaba y la copia de profundidad por CPU no se hacia (menus en negro)
static void EnsureReadback()
{
    if (g_readback) return;
    D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = 2048ull * 16 * kEdramH * ResK() * ResK(); bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES rb{ D3D12_HEAP_TYPE_READBACK };
    g_dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_readback));
}
void MemexFlushPending();
void Resolve()
{
    MemexFlushPending();   // GoWJ sesion 11: el resolve escribe memoria; antes, el memexport acumulado
    uint32_t ctrl = Reg(0x2318), destBase = Reg(0x2319), destPitchReg = Reg(0x231A), destInfo = Reg(0x231B);
    uint32_t src = ctrl & 7, cmd = (ctrl >> 20) & 3;
    bool colorClear = (ctrl >> 8) & 1;
    // Rectangulo: 3 vertices float2 del stream vf0 del propio draw
    uint32_t vdw0 = Reg(0x4800), vdw1 = Reg(0x4801);
    const uint8_t* vb = g_base + (0xA0000000u | (vdw0 & 0x1FFFFFFC));
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    for (int i = 0; i < 3; i++)
    {
        uint32_t ux = ((const uint32_t*)vb)[i * 2], uy = ((const uint32_t*)vb)[i * 2 + 1];
        if ((vdw1 & 3) == 2) { ux = __builtin_bswap32(ux); uy = __builtin_bswap32(uy); }
        float fx, fy; memcpy(&fx, &ux, 4); memcpy(&fy, &uy, 4);
        minx = std::min(minx, fx); maxx = std::max(maxx, fx); miny = std::min(miny, fy); maxy = std::max(maxy, fy);
    }
    {
        int wox, woy; WindowOffset(wox, woy);   // el rectangulo del resolve tambien se desplaza (como Xenia)
        minx += wox; maxx += wox; miny += woy; maxy += woy;
    }
    EdramRt* rt = GetEdramRt();
    if (!rt) return;
    // GoWJ sesion 3: UE3 dibuja la escena en 2_10_10_10_FLOAT (formato 3, emulado con RGBA16F) y la resuelve en bruto con una vista
    // 2_10_10_10 (formato 2) sobre la misma base de EDRAM; el tonemap la lee como fmt 54 y decodifica el 7e3 en el shader. Con
    // superficies separadas por formato el resolve leia una formato 2 vacia (imagen negra). Se resuelve desde la formato 3 y se
    // empaquetan los bits 7e3. GOWJ_NORAW7E3=1 = antes.
    bool raw7e3 = false;
    {
        static const bool noRaw = getenv("GOWJ_NORAW7E3") != nullptr;
        if (!noRaw && rt->xfmt == 2)
            for (auto& kv : g_edramRts)
                if (kv.second.xfmt == 3 && kv.second.base == rt->base && kv.second.w == rt->w) { rt = &kv.second; raw7e3 = true; break; }
    }
    // Rectangulo en pixeles logicos; en la superficie de muestras se escala por el MSAA (msx, msy)
    uint32_t msx, msy; MsaaScale(msx, msy);
    int x0 = std::max(0, (int)minx), y0 = std::max(0, (int)miny);
    uint32_t destPitch = destPitchReg & 0x3FFF, destH = (destPitchReg >> 16) & 0x3FFF;
    // Los vertices del rectangulo son inclusivos (0..1279 para 1280 de ancho): se suma 1 y se recorta al destino (sesion 21)
    const uint32_t S = rt->sx, Sy = rt->sy;   // sesion 27: escala X de la superficie (panoramico); Sy: resolucion interna
    int x1 = std::min((int)(rt->w / (msx * S)), (int)maxx + 1), y1 = std::min((int)(rt->h / (msy * Sy)), (int)maxy + 1);
    if (destPitch) x1 = std::min(x1, x0 + (int)destPitch);
    if (destH) y1 = std::min(y1, y0 + (int)destH);
    {   // diagnostico (GoWJ sesion 11): GOWJ_TDWATCH=<fisica>,<largo>: resolves que caen en ese rango
        static uint32_t lo = 0, hi = 0; static bool init = [] { if (const char* s = getenv("GOWJ_TDWATCH")) { char* e; lo = strtoul(s, &e, 16) & 0x1FFFFFFF; hi = lo + (*e == ',' ? strtoul(e + 1, nullptr, 16) : 0x1000); } return true; }(); (void)init;
        uint32_t d0 = destBase & 0x1FFFFFFF, d1 = d0 + ((destPitch + 31) & ~31u) * (((destH ? destH : (uint32_t)(maxy - miny + 1)) + 31) & ~31u) * 4;
        if (hi && d0 < hi && d1 > lo)
            LOG("[tdwatch] resolve -> %08X..%08X (pitch %u h %u info %08X) desde surf %08X rect %.0f,%.0f-%.0f,%.0f frame %llu", d0, d1, destPitch, destH, destInfo, Reg(0x2000), minx, miny, maxx, maxy, (unsigned long long)g_frameCounter);
    }
    if (S > 1)
    {   // diagnostico (GoWJ sesion 11): resolves desde superficies anchas del panoramico (texto/HUD roto solo en 32:9)
        static std::unordered_map<uint64_t, int> seenW;
        uint64_t key = (uint64_t(Reg(0x2000) & 0x3FFF) << 48) ^ (uint64_t(x1 - x0) << 32) ^ (uint64_t(y1 - y0) << 16) ^ (destInfo & 0xFFFF);
        if (seenW.size() < 400 && seenW[key]++ == 0)
            LOG("[ancho] resolve S=%u surf %08X (pitch %u, xfmt %u, rt %ux%u) rect %d,%d-%d,%d -> %08X pitch %u h %u info %08X frame %llu", S, Reg(0x2000), Reg(0x2000) & 0x3FFF, rt->xfmt, rt->w, rt->h, x0, y0, x1, y1, destBase, destPitch, destH, destInfo, (unsigned long long)g_frameCounter);
    }
    uint32_t fmt = (destInfo >> 7) & 0x3F, endian = destInfo & 7; bool swapRB = (destInfo >> 24) & 1;
    // RB_COPY_DEST_INFO bit 3 = destino 3D (array), bits 4-6 = rebanada: se escribe con el direccionamiento tiled 3D (sesion 23)
    static bool no3dResolve = getenv("GOWJ_NO3DRESOLVE") != nullptr;
    bool destArray = !no3dResolve && ((destInfo >> 3) & 1); uint32_t destSlice = (destInfo >> 4) & 7;
    int w = x1 - x0, h = y1 - y0;
    { char rb[48]; snprintf(rb, sizeof(rb), "%08X(s%u f%u e%u) ", destBase, src, fmt, endian); if (g_swapResolves.size() < 400) g_swapResolves += rb; }
    CmdTrace("resolve src %u cmd %u fmt %u rect (%d,%d)-(%d,%d) dest %08X pitch %u rt %ux%u fmt %d msaa %ux%u ctrl %08X", src, cmd, fmt, x0, y0, x1, y1, destBase, destPitch, rt->w, rt->h, (int)rt->fmt, msx, msy, ctrl);
    if ((long long)g_frameCounter == DrawLogFrame())   // sesion 24: resolves del frame registrado
        LOG("[drawresolve] src=%u cmd=%u rect=(%d,%d)-(%d,%d) -> %08X pitch %u fmt %u surf=%08X color0=%08X depth=%08X clear=%d", src, cmd, x0, y0, x1, y1, destBase, destPitch, fmt, Reg(0x2000), Reg(0x2001), Reg(0x2002), (int)colorClear);
    static std::unordered_map<uint64_t, int> seenResolve;
    uint64_t rkey = (uint64_t(src) << 56) ^ (uint64_t(fmt) << 48) ^ (uint64_t(destPitch) << 24) ^ (uint64_t(cmd) << 20) ^ (uint64_t(Reg(0x2000) >> 16) << 8);
    if (seenResolve.size() < 64 && seenResolve[rkey]++ == 0) { LOG("[edram] (msaa %u, frame %llu, formato color %u, base %u) resolve src=%u", (Reg(0x2000) >> 16) & 3, (unsigned long long)g_frameCounter, (Reg(0x2001) >> 16) & 0xF, Reg(0x2001) & 0xFFF, src); LOG("[edram] resolve src=%u cmd=%u rect=(%d,%d)-(%d,%d) -> %08X pitch %u alto %u fmt %u endian %u swap %d clear %d",
                                     src, cmd, x0, y0, x1, y1, destBase, destPitch, destH, fmt, endian, (int)swapRB, (int)colorClear); }
    // Sesion 19: resolve de profundidad (src 4) -> memoria en formato 24_8 (D24S8) tiled de 32 bits
    DepthRt* depthSrc = (src == 4 && w > 0 && h > 0 && destPitch && !g_noDepthEnv) ? GetDepthRt() : nullptr;
    {   // GOWJ_RESOLVELOG=1 (diagnostico): todos los resolves con frame y destino
        static const bool rlog = getenv("GOWJ_RESOLVELOG") != nullptr; static int rn = 0;
        static const uint32_t rlogAddr = rlog ? (uint32_t)strtoul(getenv("GOWJ_RESOLVELOG"), nullptr, 16) : 0;
        if (rlog && (rlogAddr > 1 ? destBase == rlogAddr : src == 4) && rn++ < 100000) LOG("[resolvelog] f%llu src=%u %dx%d -> %08X pitch %u fmt %u depthRt=%p surf=%08X color0=%08X depth=%08X prevPS=%016llX", (unsigned long long)g_frameCounter, src, w, h, destBase, destPitch, fmt, (void*)depthSrc,
            Reg(0x2000), Reg(0x2001), Reg(0x2002), (unsigned long long)g_curPsHash);
    }
    static bool cpuResolveDepth = getenv("GOWJ_CPURESOLVE") != nullptr || getenv("GOWJ_CPUDEPTH") != nullptr;
    if (depthSrc && g_depthCopyPso && !cpuResolveDepth)
    {
        const UINT dS = depthSrc->sx, dSy = depthSrc->sy;
        int dw = std::min(w, (int)(depthSrc->w / (msx * dS)) - x0), dhh = std::min(h, (int)(depthSrc->h / (msy * dSy)) - y0);
        if (dw > 0 && dhh > 0)
        {
            uint64_t k = ResolvedKey(destBase, dw, dhh);
            ResolvedDepth& r = g_resolvedDepth[k];
            if (r.rz && (r.sx != dS || r.sy != dSy)) { g_deferredRelease[g_fi].push_back(r.rz); g_deferredRelease[g_fi].push_back(r.rp); r.rz.Reset(); r.rp.Reset(); }
            if (!r.rz)
            {
                // GoWJ sesion 10: sin limite de 32 destinos (cada tamano de region del atlas de sombras gastaba 2 RTV para siempre; al
                // agotarse, cada resolve de profundidad iba por la CPU con una espera a la GPU: ~13.000 esperas en 3 minutos)
                D3D12_HEAP_PROPERTIES def{ D3D12_HEAP_TYPE_DEFAULT };
                D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = dw * dS; td.Height = dhh * dSy; td.DepthOrArraySize = 1; td.MipLevels = 1;
                td.SampleDesc.Count = 1; td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                td.Format = DXGI_FORMAT_R32_FLOAT;
                g_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&r.rz));
                td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                g_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&r.rp));
                if (!r.rz || !r.rp) { g_resolvedDepth.erase(k); goto depthCpu; }
                r.w = dw; r.h = dhh; r.sx = dS; r.sy = dSy; r.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
            }
            {
                D3D12_RESOURCE_BARRIER bs[3]; UINT nb = 0;
                bs[nb++] = Transition(depthSrc->res.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                if (r.state != D3D12_RESOURCE_STATE_RENDER_TARGET)
                {
                    bs[nb++] = Transition(r.rz.Get(), r.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
                    bs[nb++] = Transition(r.rp.Get(), r.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
                    r.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
                }
                ProbedBarrier(nb, bs, __LINE__);
                D3D12_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = kDepthSrvFmt; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                sd.Texture2D.MipLevels = 1; sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                // Descriptor propio del area del frame: varias copias por frame no pueden compartir hueco (la GPU los lee al ejecutar)
                UINT dslot = 6 + g_fi;
                if (g_descCursor + 1 <= kDescPerFrame) { dslot = 32 + g_fi * kDescPerFrame + g_descCursor; g_descCursor += 1; }
                g_dev->CreateShaderResourceView(depthSrc->res.Get(), &sd, SrvCpu(dslot));
                UINT inc = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
                static UINT ring2 = 0;
                auto h0 = g_rtvHeap2->GetCPUDescriptorHandleForHeapStart(); h0.ptr += ((ring2++ % 32) * 2) * inc;
                D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2] = { h0, h0 }; rtvs[1].ptr += inc;
                g_dev->CreateRenderTargetView(r.rz.Get(), nullptr, rtvs[0]); g_dev->CreateRenderTargetView(r.rp.Get(), nullptr, rtvs[1]);
                g_list->OMSetRenderTargets(2, rtvs, FALSE, nullptr);
                g_boundRt = nullptr;
                D3D12_VIEWPORT vp{ 0, 0, (float)(dw * dS), (float)(dhh * dSy), 0, 1 }; D3D12_RECT sr{ 0, 0, (LONG)(dw * dS), (LONG)(dhh * dSy) };
                g_list->RSSetViewports(1, &vp); g_list->RSSetScissorRects(1, &sr);
                g_list->SetPipelineState(g_depthCopyPso.Get());
                g_list->SetGraphicsRootSignature(g_depthCopyRootSig.Get());
                ID3D12DescriptorHeap* heaps[] = { g_srvHeap.Get() };
                g_list->SetDescriptorHeaps(1, heaps);
                int consts[4] = { x0 * (int)(msx * dS), y0 * (int)(msy * dSy), (int)msx, (int)msy };
                g_list->SetGraphicsRoot32BitConstants(0, 4, consts, 0);
                g_list->SetGraphicsRootDescriptorTable(1, SrvGpu(dslot));
                g_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                g_list->DrawInstanced(3, 1, 0, 0);
                auto back = Transition(depthSrc->res.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                ProbedBarrier(1, &back, __LINE__);
            }
            r.endian = endian; r.frame = g_frameCounter; r.screen = ScreenPitch(Reg(0x2000) & 0x3FFF);
            { static uint64_t depthGen = 0; r.gen = ++depthGen; }
            g_resolvedDepthLast[destBase & 0xFFFFF000] = k;
            g_resolvedLast.erase(destBase & 0xFFFFF000);
            g_resolves++; g_gpuResolves++;
            depthSrc = nullptr;   // hecho: no pasar por la CPU
        }
    }
depthCpu:
    if (depthSrc) EnsureReadback();
    if (depthSrc && g_readback)
    {
        FlushPendingResolves();
        g_resolvedDepthLast.erase(destBase & 0xFFFFF000);
        const UINT dS = depthSrc->sx, dSy = depthSrc->sy;
        int dw = std::min(w, (int)(depthSrc->w / (msx * dS)) - x0), dhh = std::min(h, (int)(depthSrc->h / (msy * dSy)) - y0);
        // D3D12 no permite copiar una region parcial de una textura de profundidad/stencil (E_INVALIDARG al cerrar la lista y,
        // sin capa de depuracion, GPU colgada): se copia el plano entero y se lee desde el origen (sesion 8 de NG2)
        const UINT fullW = depthSrc->w, fullH = depthSrc->h;
        UINT rowPitch = (fullW * 4 + 255) & ~255u;
        const size_t org = size_t(y0 * msy * dSy) * rowPitch + size_t(x0 * msx * dS) * 4;
        auto a = Transition(depthSrc->res.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE); ProbedBarrier(1, &a, __LINE__);
        DXGI_FORMAT planeFmt = DXGI_FORMAT_R32_TYPELESS;
        if (!g_depth32)
        {   // D24: formato copiable del plano de profundidad segun el driver (palabra de 32 bits con la profundidad en los 24 bajos)
            D3D12_RESOURCE_DESC rd = depthSrc->res->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
            g_dev->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr, nullptr);
            planeFmt = fp.Footprint.Format;
        }
        D3D12_TEXTURE_COPY_LOCATION dst{ g_readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; dst.PlacedFootprint.Footprint = { planeFmt, fullW, fullH, 1, rowPitch };
        D3D12_TEXTURE_COPY_LOCATION sl{ depthSrc->res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; sl.SubresourceIndex = 0;   // plano de profundidad
        g_list->CopyTextureRegion(&dst, 0, 0, 0, &sl, nullptr);
        auto b = Transition(depthSrc->res.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE); ProbedBarrier(1, &b, __LINE__);
        FlushAndWait();
        uint8_t* p = nullptr; g_readback->Map(0, nullptr, (void**)&p);
        if (!p) { LOG("[d3d12] Map del readback (profundidad) fallo; dispositivo: 0x%08lX", (unsigned long)g_dev->GetDeviceRemovedReason()); return; }
        uint8_t* out = g_base + (0xA0000000u | (destBase & 0x1FFFFFFF));
        uint32_t pitchAligned = (destPitch + 31) & ~31u, maxH = destH ? destH : 0x3FFF;
        for (int y = 0; y < dhh && (uint32_t)y < maxH; y++)
        {
            uint32_t outer = TiledOuter((uint32_t)y, pitchAligned, 2);
            for (int x = 0; x < dw && (uint32_t)x < destPitch; x++)
            {
                uint32_t v;
                if (g_depth32) { float z; memcpy(&z, p + org + (y * msy * dSy) * rowPitch + (x * msx * dS) * 4, 4); v = (uint32_t)(std::clamp(z, 0.0f, 1.0f) * 16777215.0f + 0.5f) << 8; }
                else { uint32_t raw; memcpy(&raw, p + org + (y * msy * dSy) * rowPitch + (x * msx * dS) * 4, 4); v = (raw & 0xFFFFFF) << 8; }
                *(uint32_t*)(out + TiledInner((uint32_t)x, (uint32_t)y, 2, outer)) = endian <= 3 ? Swap32(v, endian) : v;
            }
        }
        g_readback->Unmap(0, nullptr);
        g_resolves++;
    }
    // Bytes por texel del formato de destino del resolve (0 = sin soporte)
    uint32_t destBpp = 0;
    switch (fmt)
    {
    case 2: destBpp = 1; break;                                             // k_8
    case 3: case 4: case 10: case 15: case 24: case 30: destBpp = 2; break; // 16 bits
    case 6: case 7: case 25: case 31: case 36: destBpp = 4; break;
    case 26: case 32: case 37: destBpp = 8; break;
    }
    bool rt8888 = rt->fmt == DXGI_FORMAT_B8G8R8A8_UNORM;
    // Sesion 21: 8888 -> 8888 / k_8; otras superficies -> copia directa si el tamano del texel coincide
    // Superficies que no son 8888: se convierten a float y se empaquetan en el formato de destino (destinos de 32 y 64 bits)
    bool supported = destBpp && (rt8888 ? (fmt == 6 || fmt == 2 || fmt == 3 || fmt == 4 || fmt == 15)
                                        : (destBpp == 4 || destBpp == 8) && PackResolveSupported(fmt));
    // RB_COPY_DEST_INFO bits 16-21: sesgo de exponente con signo (el valor se multiplica por 2^sesgo)
    int expBias = (int)((destInfo >> 16) & 0x3F); if (expBias & 0x20) expBias -= 64;
    float expScale = ldexpf(1.0f, expBias);
    { static std::unordered_map<uint64_t, int> seenRb; if (expBias && seenRb.size() < 32 && seenRb[(uint64_t(destBase) << 8) | uint8_t(expBias)]++ == 0)
        LOG("[edram] resolve con sesgo de exponente %d -> %08X fmt %u (superficie formato %u, %ux%u)", expBias, destBase, fmt, rt->xfmt, w, h); }
    if (!(src == 4 && depthSrc) && !(w > 0 && h > 0 && src < 4 && cmd != 3 && supported && destPitch))
    {
        static std::unordered_map<uint32_t, int> seenSkip;
        if (seenSkip.size() < 64 && seenSkip[destBase]++ == 0)
            LOG("[edram] resolve OMITIDO -> %08X: src=%u cmd=%u rect=(%d,%d)-(%d,%d) fmt %u, superficie formato %u (bpp %u), pitch destino %u, msaa %u",
                destBase, src, cmd, x0, y0, x1, y1, fmt, rt->xfmt, rt->bpp, destPitch, (Reg(0x2000) >> 16) & 3);
    }
    static bool cpuResolve = getenv("GOWJ_CPURESOLVE") != nullptr;
    static bool fbMem = getenv("GOWJ_FBMEM") != nullptr;   // frontbuffer (endian 0) por CPU si se presenta desde memoria
    // Las copias pequenas (<= 64x64, p. ej. las rebanadas de la tabla 3D de color que el juego dibuja) van por la CPU directas a memoria:
    // asi sirven tambien a texturas 3D/cubo que leen varias copias a la vez (sesion 23: retratos negros y escenas oscuras)
    // Sesion 24 (rendimiento): ademas de 8888 -> 8888 sin MSAA, van por GPU (sin esperar a la GPU ni convertir en la CPU):
    // 8888 con MSAA (media de muestras con shader), 8888 -> k_8 y k_32_FLOAT -> k_32_FLOAT. Se leen como textura resuelta.
    // GOWJ_OLDGPURESOLVE=1 = solo el caso anterior.
    static const bool oldGpuResolve = getenv("GOWJ_OLDGPURESOLVE") != nullptr;
    uint32_t gpuTexFmt = 0; bool gpuShader = false, gpuGeneric = false;
    if (!cpuResolve && !destArray && !(fbMem && endian == 0) && w > 0 && h > 0 && src < 4 && cmd != 3 && destPitch)
    {
        bool single = msx == 1 && msy == 1;
        if (rt8888 && fmt == 6 && single) gpuTexFmt = 6;
        else if (!oldGpuResolve && rt8888 && fmt == 6 && g_colorResolvePso && g_rtvHeap3) { gpuTexFmt = 6; gpuShader = true; }
        else if (!oldGpuResolve && rt8888 && fmt == 2 && single) gpuTexFmt = 2;
        else if (!oldGpuResolve && rt->fmt == DXGI_FORMAT_R32_FLOAT && fmt == 36 && single && expBias == 0) gpuTexFmt = 36;
        else if (!oldGpuResolve && rt->fmt == DXGI_FORMAT_R16G16_SNORM && fmt == 25 && single && expBias == 0 && g_colorResolvePso1616 && g_rtvHeap3)
        { gpuTexFmt = 25; gpuShader = true; }
        // Sesion 16 de NG2 (rendimiento): RT float de 16 bits -> k_16_16_16_16_FLOAT o 8888 por la GPU. Antes iban por la CPU
        // (3 copias de 1120x584 por frame, cada una esperando a la GPU): eran ~75 % del hilo de la GPU. GOWJ_CPUFLOATRESOLVE=1 = antes.
        static const bool cpuFloat = getenv("GOWJ_CPUFLOATRESOLVE") != nullptr;
        // Sesion 19 de NG2: 8888 -> k_5_6_5 por GPU (una copia de 1280x720 por frame en combate: ~11 ms de espera). GOWJ_NOGPU565=1 = CPU
        static const bool no565 = getenv("GOWJ_NOGPU565") != nullptr;
        // GoWJ sesion 8 (rendimiento): la escena HDR (superficie 3, RGBA16F) resuelta "en bruto" a 7e3 (fmt 7) por GPU: por la CPU
        // eran ~0,9 M de pixeles convertidos y una espera a la GPU en cada cuadro (54 % del hilo de comandos). GOWJ_CPU7E3=1 = antes.
        static const bool cpu7e3 = getenv("GOWJ_CPU7E3") != nullptr;
        if (!gpuTexFmt && !oldGpuResolve && !cpu7e3 && raw7e3 && fmt == 7 && rt->fmt == DXGI_FORMAT_R16G16B16A16_FLOAT && single && g_colorResolvePso7e3 && g_rtvHeap3)
        { gpuTexFmt = 7; gpuShader = true; }
        if (!gpuTexFmt && !oldGpuResolve && !no565 && rt8888 && fmt == 4 && single && expBias == 0 && g_colorResolvePso565 && g_rtvHeap3)
        { gpuTexFmt = 4; gpuShader = true; }
        if (!gpuTexFmt && !oldGpuResolve && !cpuFloat && rt->fmt == DXGI_FORMAT_R16G16B16A16_FLOAT && expBias == 0 && g_rtvHeap3)
        {
            // GOWJ_NOGPUF32=1 / GOWJ_NOGPUF6=1: esos resolves vuelven a la CPU (sesion 17: el negro venia de la copia de region derivada)
            static const bool no32 = getenv("GOWJ_NOGPUF32") != nullptr, no6 = getenv("GOWJ_NOGPUF6") != nullptr;   // sesion 19: con los dos por GPU las capturas internas (DUMP) de menus salen negras, pero la ventana se ve bien
            if (fmt == 32 && !swapRB && g_colorResolvePso16F && !no32) { gpuTexFmt = 32; gpuShader = true; }
            else if (fmt == 6 && g_colorResolvePso && !no6) { gpuTexFmt = 6; gpuShader = true; }
        }
        // GoWJ sesion 8 (rendimiento): cadena de posproceso (bloom, DOF, luminancia) en superficies 16_16_16_16 (SNORM x32, sesgo +5)
        // o 16_16_16_16_FLOAT resuelta a 26 / 32 / 31 con sesgo -5: por GPU con conversion generica (antes ~30 resolves por CPU con
        // espera a la GPU en cada cuadro). GOWJ_CPUPOST=1 = antes.
        static const bool cpuPost = getenv("GOWJ_CPUPOST") != nullptr;
        // Con MSAA se toma la muestra 0 (como la ruta de CPU para superficies que no son 8888); tambien 2_10_10_10_FLOAT -> fmt 7.
        if (!gpuTexFmt && !oldGpuResolve && !cpuPost && !rt8888 && g_rtvHeap3 && !raw7e3 &&
            (rt->fmt == DXGI_FORMAT_R16G16B16A16_SNORM || rt->fmt == DXGI_FORMAT_R16G16B16A16_FLOAT || (rt->fmt == DXGI_FORMAT_R16G16_FLOAT && fmt == 31)) &&
            ((fmt == 26 && g_colorResolvePsoU16) || (fmt == 32 && g_colorResolvePso16F) || (fmt == 31 && g_colorResolvePso1616F) ||
             (fmt == 7 && rt->fmt == DXGI_FORMAT_R16G16B16A16_FLOAT && g_colorResolvePso7e3)))
        { gpuTexFmt = fmt; gpuShader = true; gpuGeneric = true; }
    }
    bool gpuPath = gpuTexFmt != 0;
    if (gpuPath)
    {
        uint64_t rkey2 = ResolvedKey(destBase, w, h) ^ ResolvedFmtTag(gpuTexFmt);
        ResolvedTex& r = g_resolved[rkey2];
        g_resolvedLast[destBase & 0xFFFFF000] = rkey2;
        {
            static uint32_t ageAddr = getenv("GOWJ_TEXAGE") ? (uint32_t)strtoul(getenv("GOWJ_TEXAGE"), nullptr, 16) : 0;
            static uint32_t rlogs = 0;
            if (ageAddr && (destBase & 0xFFFFF000) == ageAddr && rlogs++ < 400)
                LOG("[texage] resolve -> %08X en el frame %llu: %ux%u desde base %u (rect %d,%d-%d,%d), gpu %d", destBase, (unsigned long long)g_frameCounter, w, h, rt->base, x0, y0, x1, y1, (int)gpuPath);
        }
        DXGI_FORMAT resFmt = gpuTexFmt == 26 ? DXGI_FORMAT_R16G16B16A16_UNORM : gpuTexFmt == 31 ? DXGI_FORMAT_R16G16_FLOAT : gpuTexFmt == 7 ? DXGI_FORMAT_R10G10B10A2_UNORM : gpuTexFmt == 4 ? DXGI_FORMAT_B5G6R5_UNORM : gpuTexFmt == 32 ? DXGI_FORMAT_R16G16B16A16_FLOAT : gpuTexFmt == 36 ? DXGI_FORMAT_R32_FLOAT : gpuTexFmt == 25 ? (g_1616Sat ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R16G16_FLOAT) : DXGI_FORMAT_B8G8R8A8_UNORM;
        if (r.res && (r.fmt != resFmt || (gpuShader && r.rtv == ~0u) || r.w != (UINT)w || r.h != (UINT)h || r.sx != S || r.sy != Sy))
        {   // otro formato o hace falta poder dibujar en ella: se recrea (el recurso viejo se libera cuando la GPU acabe el frame)
            g_deferredRelease[g_fi].push_back(r.res); r.res.Reset();
        }
        if (!r.res)
        {
            D3D12_HEAP_PROPERTIES def{ D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = w * S; td.Height = h * Sy; td.DepthOrArraySize = 1; td.MipLevels = 1;
            td.Format = resFmt; td.SampleDesc.Count = 1;
            bool wantRt = gpuShader;
            // GoWJ sesion 10: los RTV ya no se asignan para siempre (se agotaban y esos resolves iban por la CPU, esperando a la GPU):
            // se crean al usarlos en un anillo (OMSetRenderTargets copia el descriptor al momento). r.rtv = 0 solo marca "puede ser RT".
            if (gpuShader && !wantRt) gpuPath = false;                                         // MSAA por la CPU
            else
            {
                if (wantRt) td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                if (FAILED(g_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r.res)))) gpuPath = false;
                else
                {
                    r.fmt = resFmt;
                    if (wantRt)
                    {
                        r.rtv = 0;
                    }
                }
            }
            r.w = w; r.h = h; r.sx = S; r.sy = Sy; r.state = D3D12_RESOURCE_STATE_COPY_DEST;
            if (!gpuPath) { g_resolvedLast.erase(destBase & 0xFFFFF000); g_resolved.erase(rkey2); }
        }
        if (gpuPath && gpuShader)
        {
            ResolvedTex& rr = g_resolved[rkey2];
            // Media de muestras con un triangulo a pantalla completa sobre la textura resuelta
            D3D12_RESOURCE_BARRIER bs[2]; UINT nb = 0;
            bs[nb++] = Transition(rt->res.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            if (rr.state != D3D12_RESOURCE_STATE_RENDER_TARGET) { bs[nb++] = Transition(rr.res.Get(), rr.state, D3D12_RESOURCE_STATE_RENDER_TARGET); rr.state = D3D12_RESOURCE_STATE_RENDER_TARGET; }
            ProbedBarrier(nb, bs, __LINE__);
            UINT dslot = 6 + g_fi;
            if (g_descCursor + 1 <= kDescPerFrame) { dslot = 32 + g_fi * kDescPerFrame + g_descCursor; g_descCursor += 1; }
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = gpuTexFmt == 25 ? DXGI_FORMAT_R16G16_SNORM : rt8888 ? DXGI_FORMAT_B8G8R8A8_UNORM : rt->fmt; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1; sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            g_dev->CreateShaderResourceView(rt->res.Get(), &sd, SrvCpu(dslot));
            UINT inc = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            static UINT ring3 = 0;
            auto h0 = g_rtvHeap3->GetCPUDescriptorHandleForHeapStart(); h0.ptr += (ring3++ % kRtvHeap3Size) * inc;
            g_dev->CreateRenderTargetView(rr.res.Get(), nullptr, h0);
            g_list->OMSetRenderTargets(1, &h0, FALSE, nullptr);
            g_boundRt = nullptr;
            D3D12_VIEWPORT vp{ 0, 0, (float)(w * S), (float)(h * Sy), 0, 1 }; D3D12_RECT sr{ 0, 0, (LONG)(w * S), (LONG)(h * Sy) };
            g_list->RSSetViewports(1, &vp); g_list->RSSetScissorRects(1, &sr);
            g_list->SetPipelineState(gpuTexFmt == 26 ? g_colorResolvePsoU16.Get() : gpuTexFmt == 31 ? g_colorResolvePso1616F.Get() : gpuTexFmt == 7 ? g_colorResolvePso7e3.Get() : gpuTexFmt == 25 ? (g_1616Sat ? g_colorResolvePso1616.Get() : g_colorResolvePso1616F.Get()) : gpuTexFmt == 32 ? g_colorResolvePso16F.Get() : gpuTexFmt == 4 ? g_colorResolvePso565.Get() : g_colorResolvePso.Get());
            g_list->SetGraphicsRootSignature(g_colorResolveRootSig.Get());
            ID3D12DescriptorHeap* heaps[] = { g_srvHeap.Get() };
            g_list->SetDescriptorHeaps(1, heaps);
            static bool noAvgS = getenv("GOWJ_NOSAMPLEAVG") != nullptr;
            int consts[8] = { x0 * (int)(msx * S), y0 * (int)(msy * Sy), (int)msx, (int)msy, noAvgS ? 0 : (int)((ctrl >> 4) & 7), gpuTexFmt == 25 ? (g_1616Sat ? 1 : 2) : gpuTexFmt == 7 ? 3 : 0, 0, 0 };
            if (gpuTexFmt == 7) { float es = expScale; memcpy(&consts[6], &es, 4); consts[7] = swapRB ? 1 : 0; }
            if (gpuGeneric)
            {   // valor real = muestra / outScale (SNORM x32); luego el sesgo de exponente del resolve
                float es = expScale / (rt->outScale != 0 ? rt->outScale : 1.0f); memcpy(&consts[6], &es, 4);
                consts[5] = 4; consts[7] = (swapRB && fmt != 31) ? 1 : 0;
            }
            g_list->SetGraphicsRoot32BitConstants(0, 8, consts, 0);
            g_list->SetGraphicsRootDescriptorTable(1, SrvGpu(dslot));
            g_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g_list->DrawInstanced(3, 1, 0, 0);
            auto back = Transition(rt->res.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            ProbedBarrier(1, &back, __LINE__);
        }
        else if (gpuPath)
        {
            ResolvedTex& rr = g_resolved[rkey2];
            if (rr.state != D3D12_RESOURCE_STATE_COPY_DEST) { auto b0 = Transition(rr.res.Get(), rr.state, D3D12_RESOURCE_STATE_COPY_DEST); ProbedBarrier(1, &b0, __LINE__); rr.state = D3D12_RESOURCE_STATE_COPY_DEST; }
            auto a = Transition(rt->res.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE); ProbedBarrier(1, &a, __LINE__);
            D3D12_BOX box{ (UINT)x0 * S, (UINT)y0 * Sy, 0, (UINT)x1 * S, (UINT)y1 * Sy, 1 };
            SafeCopyRegion(rr.res.Get(), rt->res.Get(), box, "resolve");
            auto b = Transition(rt->res.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET); ProbedBarrier(1, &b, __LINE__);
        }
        if (gpuPath)
        {
            ResolvedTex& r = g_resolved[rkey2];
            r.texFmt = gpuTexFmt;
            r.swapRB = swapRB; r.endian = endian; r.frame = g_frameCounter;
            r.destBase = destBase; r.destPitch = destPitch; r.destH = destH; r.inMemory = false; r.screen = ScreenPitch(Reg(0x2000) & 0x3FFF);
            r.memHash = MemHash(destBase); static uint64_t genCounter = 0; r.gen = ++genCounter;
            ResolveSigTake(r);   // GoWJ sesion 11
            g_gpuResolvesWin++;
            g_resolvedDepthLast.erase(destBase & 0xFFFFF000);
            g_resolves++; g_gpuResolves++;
        }
        if (gpuPath)
        {   // GoWJ sesion 10: resolve parcial (una region de la pantalla, mismo pitch) sobre una textura completa resuelta poco antes en la
            // misma direccion: en la 360 la region cae en su sitio dentro de esa textura (el juego copia solo lo que hay detras de la caja
            // de texto para desenfocarlo, 640x65 en 1280x720). Antes quedaba como textura aparte y el que leia la pantalla completa (FXAA)
            // acababa leyendo la memoria con cuadros viejos. Se copia la region dentro de la grande. GOWJ_NORESOLVEMERGE=1 = antes.
            static const bool noMerge = getenv("GOWJ_NORESOLVEMERGE") != nullptr;
            static std::unordered_map<uint32_t, uint64_t> bigKey;
            const uint32_t b = destBase & 0xFFFFF000;
            ResolvedTex& sm = g_resolved[rkey2];
            auto bk = bigKey.find(b);
            bool merged = false;
            if (!noMerge && bk != bigKey.end() && bk->second != rkey2)
            {
                auto bi = g_resolved.find(bk->second);
                if (bi != g_resolved.end() && bi->second.res && sm.res && bi->second.texFmt == sm.texFmt && bi->second.fmt == sm.fmt && bi->second.sx == sm.sx && bi->second.sy == sm.sy &&
                    bi->second.destPitch == destPitch && uint32_t(x0 + w) <= bi->second.w && uint32_t(y0 + h) <= bi->second.h && g_frameCounter - bi->second.frame <= 2)
                {
                    ResolvedTex& bg = bi->second;
                    D3D12_RESOURCE_BARRIER bs[2]; UINT nb = 0;
                    if (sm.state != D3D12_RESOURCE_STATE_COPY_SOURCE) { bs[nb++] = Transition(sm.res.Get(), sm.state, D3D12_RESOURCE_STATE_COPY_SOURCE); sm.state = D3D12_RESOURCE_STATE_COPY_SOURCE; }
                    if (bg.state != D3D12_RESOURCE_STATE_COPY_DEST) { bs[nb++] = Transition(bg.res.Get(), bg.state, D3D12_RESOURCE_STATE_COPY_DEST); bg.state = D3D12_RESOURCE_STATE_COPY_DEST; }
                    if (nb) ProbedBarrier(nb, bs, __LINE__);
                    D3D12_TEXTURE_COPY_LOCATION dl{ bg.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; dl.SubresourceIndex = 0;
                    D3D12_TEXTURE_COPY_LOCATION sl{ sm.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; sl.SubresourceIndex = 0;
                    D3D12_BOX box{ 0, 0, 0, (UINT)w * S, (UINT)h * Sy, 1 };
                    g_list->CopyTextureRegion(&dl, (UINT)x0 * S, (UINT)y0 * Sy, 0, &sl, &box);
                    bg.frame = g_frameCounter; bg.gen = sm.gen; bg.memHash = sm.memHash; bg.inMemory = false;
                    ResolveSigTake(bg);   // GoWJ sesion 11
                    g_resolvedLast[b] = bk->second;
                    merged = true;
                    static int nl = 0; if (nl++ < 8) LOG("[edram] resolve parcial %dx%d en (%d,%d) -> %08X copiado dentro de la textura de %ux%u", w, h, x0, y0, destBase, bg.w, bg.h);
                }
            }
            if (!merged)
            {
                auto bi = bk != bigKey.end() ? g_resolved.find(bk->second) : g_resolved.end();
                bool bigOk = bi != g_resolved.end() && bi->second.res && g_frameCounter - bi->second.frame <= 2;
                if (!bigOk || uint64_t(w) * h >= uint64_t(bi->second.w) * bi->second.h) bigKey[b] = rkey2;
            }
        }
    }
    if (!gpuPath && w > 0 && h > 0 && src < 4 && cmd != 3 && supported && destPitch)
    {
        // Un resolve por CPU a esta direccion invalida la copia en la GPU (sin liberar el recurso: la GPU puede estar usandolo)
        g_resolvedLast.erase(destBase & 0xFFFFF000);
        g_cpuResolveGen[destBase & 0xFFFFF000] = ++g_cpuResolveCounter;
        g_cpuResolvesWin++;
        static const bool cpuStats = getenv("GOWJ_CPURESOLVESTATS") != nullptr;
        if (cpuStats)
        {   // estadistica (sesion 24): por que va por la CPU
            static std::unordered_map<std::string, uint64_t> why; static uint64_t total = 0;
            char k[160]; snprintf(k, sizeof(k), "%ux%u fmt%u sup%u arr%d msaa%ux%u dest%08X", w, h, fmt, rt->xfmt, (int)destArray, msx, msy, destBase);
            why[k]++;
            if (++total % 160 == 0)
            {
                std::vector<std::pair<uint64_t, std::string>> v; for (auto& kv : why) v.push_back({ kv.second, kv.first });
                std::sort(v.rbegin(), v.rend());
                for (size_t i = 0; i < v.size() && i < 12; i++) LOG("[cpuresolve] %llu x %s", (unsigned long long)v[i].first, v[i].second.c_str());
                why.clear();
            }
        }
        if (!g_readback)
        {
            D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = 2048ull * 16 * kEdramH * ResK() * ResK(); bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
            bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            D3D12_HEAP_PROPERTIES rb{ D3D12_HEAP_TYPE_READBACK };
            g_dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_readback));
        }
        const UINT msxS = msx * S, msyS = msy * Sy;   // sesion 27: en la superficie panoramica cada pixel del juego son msx * S columnas (y msy * Sy filas)
        UINT rowPitch = ((UINT)(w * msxS) * rt->bpp + 255) & ~255u;
        // Desactivado por defecto: con el diferido el juego se cerraba al cargar un escenario (las copias tardias llegan
        // despues de que el juego reutiliza esa memoria). GOWJ_DEFERRESOLVE=1 lo activa (experimental).
        static const bool noDefer = getenv("GOWJ_DEFERRESOLVE") == nullptr;
        UINT64 need = UINT64(rowPitch) * (UINT64)(h * msyS);
        bool defer = !noDefer && need + 512 <= kReadbackDeferSize;
        if (defer && !g_readbackDefer)
        {
            D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = kReadbackDeferSize; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
            bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            D3D12_HEAP_PROPERTIES rbh{ D3D12_HEAP_TYPE_READBACK };
            if (FAILED(g_dev->CreateCommittedResource(&rbh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_readbackDefer)))) defer = false;
        }
        UINT64 rbOff = 0;
        if (defer)
        {
            if (g_readbackDeferOff + need + 512 > kReadbackDeferSize) FlushPendingResolves();
            rbOff = (g_readbackDeferOff + 511) & ~511ull;
            g_readbackDeferOff = rbOff + need;
        }
        else FlushPendingResolves();
        auto a = Transition(rt->res.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE); ProbedBarrier(1, &a, __LINE__);
        D3D12_TEXTURE_COPY_LOCATION dst{ defer ? g_readbackDefer.Get() : g_readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; dst.PlacedFootprint.Footprint = { rt->fmt, (UINT)(w * msxS), (UINT)(h * msyS), 1, rowPitch };
        dst.PlacedFootprint.Offset = rbOff;
        D3D12_TEXTURE_COPY_LOCATION sl{ rt->res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; sl.SubresourceIndex = 0;
        D3D12_BOX box{ (UINT)(x0 * msxS), (UINT)(y0 * msyS), 0, (UINT)(x1 * msxS), (UINT)(y1 * msyS), 1 };
        g_list->CopyTextureRegion(&dst, 0, 0, 0, &sl, &box);
        auto b = Transition(rt->res.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET); ProbedBarrier(1, &b, __LINE__);
        const UINT rtBpp = rt->bpp; const DXGI_FORMAT rtFmt = rt->fmt;
        auto convert = [=](const uint8_t* p) {
        uint8_t* out = g_base + (0xA0000000u | (destBase & 0x1FFFFFFF));
        uint32_t pitchAligned = (destPitch + 31) & ~31u;
        uint32_t maxH = destH ? destH : 0x3FFF;
        uint32_t log2d = destBpp == 1 ? 0 : destBpp == 2 ? 1 : destBpp == 4 ? 2 : 3;
        for (int y = 0; y < h && (uint32_t)y < maxH; y++)
        {
            uint32_t outer = TiledOuter((uint32_t)y, pitchAligned, log2d);
            for (int x = 0; x < w && (uint32_t)x < destPitch; x++)
            {
                const uint8_t* px = p + (y * msyS) * rowPitch + (x * msxS) * rtBpp;   // una muestra por pixel
                // Sesion 24 (como Xenia): RB_COPY_CONTROL bits 4-6 = muestra a copiar; 4 = media de 0/1, 5 = media de 2/3,
                // 6 = media de las 4. El juego reduce la escena a la mitad reinterpretandola como MSAA 4x con media.
                uint8_t avgPx[16];
                static bool noAvg = getenv("GOWJ_NOSAMPLEAVG") != nullptr;
                uint32_t sampleSel = (ctrl >> 4) & 7;
                if (!noAvg && rt8888 && sampleSel >= 4 && sampleSel <= 6 && (msx > 1 || msy > 1))
                {
                    // Muestras en la superficie: (0,0) (1,0) (0,1) (1,1) dentro del bloque msx x msy
                    const uint8_t* s[4] = { px, px + (msx > 1 ? 4 : 0), px + (msy > 1 ? rowPitch * Sy : 0), px + (msy > 1 ? rowPitch * Sy : 0) + (msx > 1 ? 4 : 0) };
                    int first = sampleSel == 5 ? 2 : 0, count = sampleSel == 6 ? 4 : 2;
                    if (sampleSel == 4 && msy > 1 && msx == 1) { s[1] = s[2]; }   // 2x: muestras arriba/abajo
                    for (int c = 0; c < 4; c++)
                    {
                        uint32_t sum = 0;
                        for (int k = 0; k < count; k++) sum += s[first + k][c];
                        avgPx[c] = (uint8_t)((sum + count / 2) / count);
                    }
                    px = avgPx;
                }
                uint8_t* o = out + (destArray ? (uint64_t)Tiled3DOffset(x, y, (int32_t)destSlice, pitchAligned, (destH + 31) & ~31u, log2d)
                                              : (uint64_t)TiledInner((uint32_t)x, (uint32_t)y, log2d, outer));
                if (rt8888 && fmt == 2) { *o = px[2]; continue; }   // k_8: canal rojo
                if (rt8888 && destBpp == 2)
                {
                    // 8888 -> 16 bits (sesion 21). px = B G R A. Con swapRB se intercambian R y B.
                    uint32_t r = swapRB ? px[0] : px[2], g = px[1], b = swapRB ? px[2] : px[0], a = px[3];
                    uint16_t v = fmt == 3 ? (uint16_t)(((a >> 7) << 15) | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3))      // 1_5_5_5
                               : fmt == 4 ? (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))                       // 5_6_5
                               : (uint16_t)(((a >> 4) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4));                // 4_4_4_4
                    // El endian del destino se aplica por palabras de 16 bits (8in16 = intercambio de bytes)
                    if (endian == 1 || endian == 2) v = __builtin_bswap16(v);
                    *(uint16_t*)o = v;
                    continue;
                }
                if (rt8888)
                {
                    uint32_t v = swapRB ? (px[0] | (px[1] << 8) | (px[2] << 16) | (px[3] << 24))   // bytes B G R A
                                        : (px[2] | (px[1] << 8) | (px[0] << 16) | (px[3] << 24));  // bytes R G B A
                    *(uint32_t*)o = endian <= 3 ? Swap32(v, endian) : v;
                    continue;
                }
                // Sesion 23 (como resolve.xesli de Xenia): se desempaqueta el valor del render target a float y se empaqueta en el
                // formato de destino con su sesgo de exponente. 16_16 y 16_16_16_16 son de punto fijo -32..32 (nuestro SNORM x32)
                // y el resolve escribe datos SIN signo: no es una copia bit a bit.
                static bool raw16 = getenv("GOWJ_RAWRESOLVE") != nullptr;   // diagnostico: copia bit a bit (comportamiento anterior)
                if (raw16 && rtBpp == destBpp)
                {
                    for (uint32_t k = 0; k < destBpp / 4; k++) ((uint32_t*)o)[k] = Swap32(((const uint32_t*)px)[k], endian);
                    continue;
                }
                // Sesion 16 de NG2: mismo formato half (RT de 16 bits float -> k_16_16_16_16_FLOAT, sin sesgo): copia directa de los 16 bits
                if (rtFmt == DXGI_FORMAT_R16G16B16A16_FLOAT && fmt == 32 && expScale == 1.0f)
                {
                    uint16_t hv[4]; memcpy(hv, px, 8);
                    if (swapRB) std::swap(hv[0], hv[2]);
                    uint32_t w0 = hv[0] | (uint32_t(hv[1]) << 16), w1 = hv[2] | (uint32_t(hv[3]) << 16);
                    ((uint32_t*)o)[0] = Swap32(w0, endian); ((uint32_t*)o)[1] = Swap32(w1, endian);
                    continue;
                }
                float v[4];
                UnpackRtPixel(rtFmt, px, v);
                for (int k = 0; k < 4; k++) v[k] *= expScale;
                uint32_t words[2] = {};
                // copy_dest_swap: tambien desde render targets float (la escena HDR de NG2 salia azul en vez de roja, sesion 10)
                if (swapRB && (fmt == 6 || fmt == 7 || fmt == 26 || fmt == 32)) std::swap(v[0], v[2]);
                if (raw7e3 && fmt == 7)
                {
                    words[0] = Float32To7e3(v[0]) | (Float32To7e3(v[1]) << 10) | (Float32To7e3(v[2]) << 20) |
                               ((uint32_t)(std::clamp(std::isnan(v[3]) ? 0.0f : v[3], 0.0f, 1.0f) * 3.0f + 0.5f) << 30);
                    static const bool st7 = getenv("GOWJ_7E3STATS") != nullptr;   // sesion 8: diagnostico (contadores por pixel)
                    static std::atomic<uint64_t> n7{ 0 }, nz7{ 0 }; static std::atomic<float> mx7{ 0 };
                    if (st7) { uint64_t k = n7++; if (words[0] & 0x3FFFFFFF) nz7++; if (v[1] > mx7) mx7 = v[1];
                    if (k % 4000000 == 3999999) LOG("[7e3] %llu pixeles, %llu no nulos, G max %g, ultimo %08X (v %g %g %g %g) endian %u", (unsigned long long)k + 1, (unsigned long long)nz7.load(), (double)mx7.load(), words[0], v[0], v[1], v[2], v[3], endian); }
                }
                else
                PackResolvePixel(fmt, v, words);
                for (uint32_t k = 0; k < destBpp / 4; k++) ((uint32_t*)o)[k] = Swap32(words[k], endian);
            }
        }
        };
        if (defer)
        {
            uint32_t span = ((destPitch + 31) & ~31u) * (((destH ? destH : (uint32_t)h) + 31) & ~31u) * std::max<uint32_t>(destBpp, 1);
            if (destArray) span = 0x01000000;   // texturas 3D: rango amplio (conservador)
            uint32_t lo = destBase & 0x1FFFFFFF;
            g_pendingResolves.push_back({ rbOff, lo, lo + std::max<uint32_t>(span, 4096), convert, DeferSig(lo, lo + std::max<uint32_t>(span, 4096)), g_frameCounter });
            static const bool deferNow = getenv("GOWJ_DEFERNOW") != nullptr;   // diagnostico: completar al momento
            if (deferNow) FlushPendingResolves();
        }
        else
        {
            FlushAndWait();
            uint8_t* p = nullptr; g_readback->Map(0, nullptr, (void**)&p);
            if (!p)
            {
                static int n = 0;
                if (n++ < 3) LOG("[d3d12] Map del readback fallo; dispositivo: 0x%08lX", (unsigned long)g_dev->GetDeviceRemovedReason());
                if (n == 1) DumpDred();
                return;
            }
            convert(p);
            g_readback->Unmap(0, nullptr);
        }
        g_resolves++;
        if ((destBase & 0x1FFFFFFF) == (GpuGetStats().frontbuffer.load() & 0x1FFFFFFF) || destBase == 0x17DF0000) g_fbResolves++;
    }
    if (colorClear && w > 0 && h > 0)
    {
        uint32_t c = Reg(0x231E);
        float col[4] = { ((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f, (c >> 24) / 255.0f };
        D3D12_RECT r{ (LONG)(x0 * msx * S), (LONG)(y0 * msy * Sy), (LONG)(x1 * msx * S), (LONG)std::min<uint32_t>(y1 * msy * Sy, rt->h) };
        g_list->ClearRenderTargetView(Rtv(rt->rtv), col, 1, &r);
    }
    // Clear de profundidad/stencil (RB_COPY_CONTROL bit 9, valor en RB_DEPTH_CLEAR 0x231D)
    if (((ctrl >> 9) & 1) && w > 0 && h > 0 && !g_noDepthEnv)
    {
        if (DepthRt* dr = GetDepthRt())
        {
            uint32_t v = Reg(0x231D);
            float z = DecodeDepth24(v >> 8, (Reg(0x2002) >> 16) & 1);
            D3D12_RECT r{ (LONG)(x0 * msx * dr->sx), (LONG)(y0 * msy * dr->sy), std::min((LONG)(x1 * msx * dr->sx), (LONG)dr->w), std::min((LONG)(y1 * msy * dr->sy), (LONG)dr->h) };
            g_list->ClearDepthStencilView(Dsv(dr->dsv), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, z, (UINT8)(v & 0xFF), 1, &r);
            static int logged = 0;
            if (logged < 6) { logged++; LOG("[edram] clear de profundidad %08X -> z=%g stencil=%u rect=(%d,%d)-(%d,%d)", v, z, v & 0xFF, x0, y0, x1, y1); }
        }
    }
}

// Vuelca a la memoria del juego una textura resuelta por GPU (como el resolve por CPU: tiled 8888 con swapRB y endian del destino)
static void WriteBackResolved(ResolvedTex& r)
{
    if (!g_readback)
    {   // sesion 17 de NG2: con los resolves por GPU puede no haberse creado aun (lo creaba el resolve por CPU)
        D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = 2048ull * 16 * kEdramH * ResK() * ResK(); bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES rb{ D3D12_HEAP_TYPE_READBACK };
        g_dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_readback));
    }
    if (!r.res || r.inMemory || !g_readback || !r.destPitch) { static int n = 0; if (n++ < 6) LOG("[writeback] omitido: res %d enMemoria %d readback %d pitch %u", (int)(r.res != nullptr), (int)r.inMemory, (int)(g_readback != nullptr), r.destPitch); return; }
    FlushPendingResolves();
    if (r.state != D3D12_RESOURCE_STATE_COPY_SOURCE) { auto b = Transition(r.res.Get(), r.state, D3D12_RESOURCE_STATE_COPY_SOURCE); ProbedBarrier(1, &b, __LINE__); r.state = D3D12_RESOURCE_STATE_COPY_SOURCE; }
    const UINT bpp = (r.texFmt == 32 || r.texFmt == 26) ? 8u : r.texFmt == 4 ? 2u : 4u;   // sesion 16 de NG2: k_16_16_16_16_FLOAT (8 bytes por pixel); 19: 5_6_5
    const UINT rsx = r.sx ? r.sx : 1;   // sesion 27: recurso panoramico (w * sx de ancho): se toma una columna de cada sx
    const UINT rsy = r.sy ? r.sy : 1;   // GoWJ sesion 11: y una fila de cada sy (resolucion interna)
    UINT rowPitch = (r.w * rsx * bpp + 255) & ~255u;
    D3D12_TEXTURE_COPY_LOCATION dst{ g_readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; dst.PlacedFootprint.Footprint = { r.fmt, r.w * rsx, r.h * rsy, 1, rowPitch };
    D3D12_TEXTURE_COPY_LOCATION sl{ r.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; sl.SubresourceIndex = 0;
    g_list->CopyTextureRegion(&dst, 0, 0, 0, &sl, nullptr);
    FlushAndWait();
    uint8_t* p = nullptr; g_readback->Map(0, nullptr, (void**)&p);
    if (!p) return;
    uint8_t* out = g_base + (0xA0000000u | (r.destBase & 0x1FFFFFFF));
    uint32_t pitchAligned = (r.destPitch + 31) & ~31u, maxH = r.destH ? r.destH : 0x3FFF;
    if (r.texFmt == 4)
    {   // 5_6_5 (sesion 19): palabra de 16 bits; con swapRB se intercambian R y B; endian por palabras de 16 bits
        for (uint32_t y = 0; y < r.h && y < maxH; y++)
        {
            uint32_t outer = TiledOuter(y, pitchAligned, 1);
            for (uint32_t x = 0; x < r.w && x < r.destPitch; x++)
            {
                uint16_t v = *(const uint16_t*)(p + y * rsy * rowPitch + x * rsx * 2);
                if (r.swapRB) v = (uint16_t)(((v & 0x1F) << 11) | (v & 0x07E0) | (v >> 11));
                if (r.endian == 1 || r.endian == 2) v = __builtin_bswap16(v);
                *(uint16_t*)(out + TiledInner(x, y, 1, outer)) = v;
            }
        }
        g_readback->Unmap(0, nullptr); if (r.wlen) ResolveSigTake(r);   /* GoWJ sesion 11: el volcado propio no es la CPU del juego */
        r.inMemory = true; g_cpuResolveCounter++; g_writeBacks++;
        return;
    }
    if (r.texFmt == 32 || r.texFmt == 26)
    {   // dos palabras (R | G << 16, B | A << 16) con el endian del destino, tiling de 8 bytes
        for (uint32_t y = 0; y < r.h && y < maxH; y++)
        {
            uint32_t outer = TiledOuter(y, pitchAligned, 3);
            for (uint32_t x = 0; x < r.w && x < r.destPitch; x++)
            {
                const uint32_t* px = (const uint32_t*)(p + y * rsy * rowPitch + x * rsx * 8);
                uint32_t* o = (uint32_t*)(out + TiledInner(x, y, 3, outer));
                o[0] = Swap32(px[0], r.endian); o[1] = Swap32(px[1], r.endian);
            }
        }
        g_readback->Unmap(0, nullptr); if (r.wlen) ResolveSigTake(r);   /* GoWJ sesion 11: el volcado propio no es la CPU del juego */
        r.inMemory = true; g_cpuResolveCounter++; g_writeBacks++;
        return;
    }
    if (r.texFmt == 2 || r.texFmt == 36 || r.texFmt == 25 || r.texFmt == 7 || r.texFmt == 31)
    {   // sesion 24: k_8 = canal rojo (byte 2 de BGRA) con tiling de 1 byte; k_32_FLOAT = palabra con el endian del destino
        for (uint32_t y = 0; y < r.h && y < maxH; y++)
        {
            uint32_t outer = TiledOuter(y, pitchAligned, r.texFmt == 2 ? 0 : 2);
            for (uint32_t x = 0; x < r.w && x < r.destPitch; x++)
            {
                const uint8_t* px = p + y * rsy * rowPitch + x * rsx * 4;
                if (r.texFmt == 2) *(out + TiledInner(x, y, 0, outer)) = px[2];
                else if (r.texFmt == 25 && r.fmt == DXGI_FORMAT_R16G16_FLOAT)
                {   // 16_16 sin recortar (sesion 24): en memoria, entero de 16 bits con 0x7FFF = 32 (como Xenia)
                    uint32_t wv = 0;
                    for (int k = 0; k < 2; k++)
                    {
                        float fv = HalfToFloat(((const uint16_t*)px)[k]);
                        int iv = (int)std::lround(std::clamp(fv / 32.0f, -1.0f, 1.0f) * 32767.0f);
                        wv |= uint32_t(uint16_t(int16_t(iv))) << (16 * k);
                    }
                    *(uint32_t*)(out + TiledInner(x, y, 2, outer)) = Swap32(wv, r.endian);
                }
                else *(uint32_t*)(out + TiledInner(x, y, 2, outer)) = Swap32(*(const uint32_t*)px, r.endian);   // 32_FLOAT y 16_16 (R | G << 16)
            }
        }
        g_readback->Unmap(0, nullptr); if (r.wlen) ResolveSigTake(r);   /* GoWJ sesion 11: el volcado propio no es la CPU del juego */
        r.inMemory = true;
        g_cpuResolveCounter++;
        g_writeBacks++;
        return;
    }
    for (uint32_t y = 0; y < r.h && y < maxH; y++)
    {
        uint32_t outer = TiledOuter(y, pitchAligned, 2);
        for (uint32_t x = 0; x < r.w && x < r.destPitch; x++)
        {
            const uint8_t* px = p + y * rsy * rowPitch + x * rsx * 4;
            uint32_t v = r.swapRB ? (px[0] | (px[1] << 8) | (px[2] << 16) | (px[3] << 24)) : (px[2] | (px[1] << 8) | (px[0] << 16) | (px[3] << 24));
            *(uint32_t*)(out + TiledInner(x, y, 2, outer)) = r.endian <= 3 ? Swap32(v, r.endian) : v;
        }
    }
    g_readback->Unmap(0, nullptr); if (r.wlen) ResolveSigTake(r);   /* GoWJ sesion 11: el volcado propio no es la CPU del juego */
    r.inMemory = true;
    g_writeBacks++;
    g_cpuResolveCounter++;   // la memoria cambio: invalida las huellas de textura memorizadas en este frame
    static int n = 0;
    if (n++ < 20) LOG("[d3d12] textura resuelta %08X (%ux%u) volcada a memoria para una lectura con otro formato/tamano", r.destBase, r.w, r.h);
}
static long long DrawLogFrame();
static long long DrawLogFrameExt() { return DrawLogFrame(); }
// Diagnostico (sesion 23): guarda una textura resuelta por GPU como BMP (BGRA tal cual, con canal alfa aparte en otro BMP)
static void DumpResolvedTex(ResolvedTex& r, const char* tag)
{
    if (!r.res || !g_readback) return;
    if (r.state != D3D12_RESOURCE_STATE_COPY_SOURCE) { auto b = Transition(r.res.Get(), r.state, D3D12_RESOURCE_STATE_COPY_SOURCE); ProbedBarrier(1, &b, __LINE__); r.state = D3D12_RESOURCE_STATE_COPY_SOURCE; }
    const UINT rw = r.w * (r.sx ? r.sx : 1);
    // Sesion 10: tambien texturas de 8 bytes por pixel (16_16_16_16 UNORM / FLOAT): se pasan a 8 bits por canal
    const bool wide = r.fmt == DXGI_FORMAT_R16G16B16A16_UNORM || r.fmt == DXGI_FORMAT_R16G16B16A16_FLOAT;
    const UINT pbytes = wide ? 8u : 4u;
    UINT rowPitch = (rw * pbytes + 255) & ~255u;
    D3D12_TEXTURE_COPY_LOCATION dst{ g_readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; dst.PlacedFootprint.Footprint = { r.fmt, rw, r.h * (r.sy ? r.sy : 1), 1, rowPitch };
    D3D12_TEXTURE_COPY_LOCATION sl{ r.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; sl.SubresourceIndex = 0;
    g_list->CopyTextureRegion(&dst, 0, 0, 0, &sl, nullptr);
    FlushAndWait();
    uint8_t* p = nullptr; g_readback->Map(0, nullptr, (void**)&p);
    if (!p) return;
    const UINT rsx = r.sx ? r.sx : 1;
    for (int pass = 0; pass < 2; pass++)
    {
        char name[96]; snprintf(name, sizeof(name), "res_%s_%08X%s.bmp", tag, r.destBase, pass ? "_alfa" : "");
        if (FILE* f = fopen(name, "wb"))
        {
            uint32_t size = r.w * r.h * 4;
            uint8_t hdr[54] = { 'B', 'M' }; *(uint32_t*)(hdr + 2) = 54 + size; *(uint32_t*)(hdr + 10) = 54; *(uint32_t*)(hdr + 14) = 40;
            *(int32_t*)(hdr + 18) = (int32_t)r.w; *(int32_t*)(hdr + 22) = -(int32_t)r.h; *(uint16_t*)(hdr + 26) = 1; *(uint16_t*)(hdr + 28) = 32; *(uint32_t*)(hdr + 34) = size;
            fwrite(hdr, 1, 54, f);
            std::vector<uint8_t> row(r.w * 4);
            for (UINT y = 0; y < r.h; y++)
            {
                const uint8_t* s = p + y * (r.sy ? r.sy : 1) * rowPitch;
                for (UINT x = 0; x < r.w; x++)
                {
                    if (wide)
                    {   // RGBA de 16 bits -> BGRA de 8 bits
                        const uint16_t* q = (const uint16_t*)(s + x * rsx * 8);
                        auto c8 = [&](uint16_t v) -> uint8_t { if (r.fmt == DXGI_FORMAT_R16G16B16A16_UNORM) return uint8_t(v >> 8); float f = HalfToFloat(v); return uint8_t(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f); };
                        if (pass) row[x*4] = row[x*4+1] = row[x*4+2] = c8(q[3]); else { row[x*4] = c8(q[2]); row[x*4+1] = c8(q[1]); row[x*4+2] = c8(q[0]); }
                        row[x*4+3] = 255; continue;
                    }
                    const uint8_t* s0 = s; const uint8_t* s = s0 + x * rsx * 4 - x * 4; uint8_t a = s[x * 4 + 3]; if (pass) { row[x*4] = row[x*4+1] = row[x*4+2] = a; } else { row[x*4] = s[x*4]; row[x*4+1] = s[x*4+1]; row[x*4+2] = s[x*4+2]; } row[x*4+3] = 255;
                }
                fwrite(row.data(), 1, row.size(), f);
            }
            fclose(f);
        }
    }
    g_readback->Unmap(0, nullptr);
}

// Antes de preparar un draw: las texturas que el camino rapido no puede servir desde la GPU se vuelcan a memoria
static void WriteBackResolvedForDraw(uint32_t mask, uint32_t maskMem = 0)   // maskMem: slots que se leen desde memoria (3D/cubo)
{
    for (uint32_t t = 0; t < 16; t++)
    {
        if (!(mask & (1u << t))) continue;
        uint32_t fc0 = Reg(0x4800 + t * 6), fc1 = Reg(0x4800 + t * 6 + 1), fc2 = Reg(0x4800 + t * 6 + 2), fc5 = Reg(0x4800 + t * 6 + 5);
        if ((fc0 & 3) != 2) continue;
        uint32_t base = fc1 & 0xFFFFF000, fmt = fc1 & 0x3F, dim = (fc5 >> 9) & 3;
        PruneStaleResolve(base);   // GoWJ sesion 11: no volcar una pantalla vieja encima de lo que la CPU ha cargado despues
        auto last = g_resolvedLast.find(base);
        if (last == g_resolvedLast.end()) continue;
        auto it = g_resolved.find(last->second);
        if (it == g_resolved.end() || it->second.inMemory) continue;
        if ((maskMem & (1u << t)) && GpuLutSource(t)) continue;   // GoWJ sesion 8: LUT 3D copiada en la GPU (sin volcado)
        ResolvedTex& r = it->second;
        uint32_t w = (fc2 & 0x1FFF) + 1, h = ((fc2 >> 13) & 0x1FFF) + 1;
        static const bool depthWbOld = getenv("GOWJ_DEPTHWBOLD") != nullptr;
        if (!depthWbOld && (fmt == 22 || fmt == 23) && dim == 1)
        {   // sesion 10: profundidad (atlas de sombras) con un resolve de profundidad mas reciente que el de color en esta direccion:
            // la sirve BindTexture desde la GPU. Antes se volcaba el color del cuadro anterior a memoria en cada cuadro (espera a la GPU).
            auto dl = g_resolvedDepthLast.find(base);
            auto di = dl != g_resolvedDepthLast.end() ? g_resolvedDepth.find(dl->second) : g_resolvedDepth.end();
            if (di != g_resolvedDepth.end() && di->second.rz && di->second.frame >= r.frame) continue;
        }
        {   // sesion 10: hay un resolve de este cuadro con el tamano y formato exactos: BindTexture lo usa, no hace falta volcar
            uint32_t rf = fmt == 29 ? 32u : fmt == 54 ? 7u : fmt == 28 ? 31u : fmt;
            if (dim == 1 && !(maskMem & (1u << t)) && last->second != (ResolvedKey(base, w, h) ^ ResolvedFmtTag(rf)) && ExactRecentResolve(base, w, h, rf) != g_resolved.end()) continue;
        }
        {
            // GOWJ_DUMPRES=<direccion hex>: en el frame de GOWJ_DRAWLOG, vuelca la copia por GPU que leera este draw
            static uint32_t dumpAddr = getenv("GOWJ_DUMPRES") ? (uint32_t)strtoul(getenv("GOWJ_DUMPRES"), nullptr, 16) : 0;
            static int dumps = 0;
            if (dumpAddr == base && dumps < 12 && (long long)g_frameCounter == DrawLogFrameExt())
            {
                char tag[32]; snprintf(tag, sizeof(tag), "n%02d_g%llu", dumps++, (unsigned long long)r.gen);
                DumpResolvedTex(r, tag);
            }
        }
        static const bool as54w = getenv("GOWJ_54AS8888") != nullptr, wb32 = getenv("GOWJ_WB32") != nullptr, noNear32w = getenv("GOWJ_NONEAR32") != nullptr;
        bool gpuOk = !(maskMem & (1u << t)) && dim == 1 && (((fmt == 6 || ((fmt == 7 || fmt == 54) && as54w)) && r.texFmt == 6 && DynResFits(w, h, r.w, r.h)) ||     // directo o region derivada (GoWJ: 7/54)
                                  ((fmt == 2 || fmt == 36 || (fmt == 25 && ((fc0 >> 2) & 0x55) == 0 && ((Reg(0x4800 + t * 6 + 3) >> 13) & 0x3F) == 0)) &&
                                   r.texFmt == fmt && w <= r.w + 8 && h <= r.h + 8) ||   // sesion 24 (directo o region derivada)
                                  ((fmt == 32 || fmt == 29) && r.texFmt == 32 && w <= r.w && h <= r.h && w + 8 >= r.w && h + 8 >= r.h && !wb32 && !noNear32w) ||   // sesion 16-19 de NG2
                                  (fmt == 4 && r.texFmt == 4 && w == r.w && h == r.h) ||
                                  ((fmt == 7 || fmt == 54) && r.texFmt == 7 && DynResFits(w, h, r.w, r.h)) ||   // GoWJ sesion 8
                                  (fmt == 26 && r.texFmt == 26 && ((fc0 >> 2) & 0x55) == 0 && ((Reg(0x4800 + t * 6 + 3) >> 13) & 0x3F) == 0 && w <= r.w + 8 && h <= r.h + 8) ||
                                  ((fmt == 31 || fmt == 28) && r.texFmt == 31 && w <= r.w + 8 && h <= r.h + 8));   // sesion 19
        if (!gpuOk)
        {
            static std::unordered_map<uint64_t, int> seenWb;
            uint64_t wk = (uint64_t(base) << 16) ^ (uint64_t(fmt) << 8) ^ dim ^ (uint64_t(w) << 40) ^ (uint64_t(r.texFmt) << 2);
            if (!r.inMemory && seenWb.size() < 40 && seenWb[wk]++ == 0)
                LOG("[writeback] %08X leida como fmt %u dim %u %ux%u; resuelta como texFmt %u %ux%u (PS %016llX)", base, fmt, dim, w, h, r.texFmt, r.w, r.h, (unsigned long long)g_curPsHash);
            WriteBackResolved(r);
        }
    }
}

// GOWJ_DRAWLOG=<frame> o t<segundos> (primer frame tras ese tiempo desde el arranque)
static long long g_forceLogFrame = -1;   // sesion 24: frame forzado por GOWJ_DUMPVS
static long long DrawLogFrame()
{
    if (g_forceLogFrame >= 0 && (long long)g_frameCounter == g_forceLogFrame) return g_forceLogFrame;
    static const char* s = getenv("GOWJ_DRAWLOG");
    static long long frame = (s && s[0] != 't' && s[0] != 's') ? atoll(s) : -1;
    static DWORD t0 = GetTickCount();
    if (s && s[0] == 't' && frame < 0 && GetTickCount() - t0 >= (DWORD)(atof(s + 1) * 1000)) frame = (long long)g_frameCounter + 1;
    // GOWJ_DRAWLOG=s<RB_SURFACE_INFO hex>: el frame siguiente al primero que dibuja en esa superficie (sesion 23)
    if (s && s[0] == 's' && frame < 0 && Reg(0x2000) == (uint32_t)strtoul(s + 1, nullptr, 16)) frame = (long long)g_frameCounter + 1;
    // GOWJ_DRAWLOGN=n: registra n frames seguidos (el juego reparte escena y postproceso en dos swaps) (sesion 24)
    static long long n = getenv("GOWJ_DRAWLOGN") ? atoll(getenv("GOWJ_DRAWLOGN")) : 1;
    // Sesion 24: F9 (una comprobacion por frame) registra los 2 frames siguientes (los dos tipos de frame del juego)
    {
        static uint64_t lastCheck = ~0ull; static bool wasDown = false;
        if (lastCheck != g_frameCounter)
        {
            lastCheck = g_frameCounter;
            DWORD fgPid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &fgPid);   // F9 solo con la ventana del juego activa (antes saltaba desde otras aplicaciones)
            bool down = (fgPid == GetCurrentProcessId() && (GetAsyncKeyState(VK_F9) & 0x8000) != 0) || g_scriptF9.exchange(false);   // GoWJ sesion 11: DUMP del guion con GOWJ_DUMPLOG=1
            if (down && !wasDown && (frame < 0 || (long long)g_frameCounter >= frame + n))
            { frame = (long long)g_frameCounter + 1; n = 2; LOG("[drawlog] F9: registrando los frames %lld y %lld", frame, frame + 1); }
            wasDown = down;
            // GOWJ_DRAWLOGEVERY=<desde s>:<cada s>: registra 2 frames y guarda shot_<s>.bmp periodicamente (diagnostico, sesion 24)
            static const char* ev = getenv("GOWJ_DRAWLOGEVERY");
            if (ev)
            {
                static double from = atof(ev), every = strchr(ev, ':') ? atof(strchr(ev, ':') + 1) : 5.0;
                static DWORD start = GetTickCount(); static double next = from;
                double el = (GetTickCount() - start) / 1000.0;
                if (el >= next && (frame < 0 || (long long)g_frameCounter >= frame + n))
                {
                    next += every; frame = (long long)g_frameCounter + 1; n = 2;
                    extern std::atomic<int> g_dumpRequest; g_dumpRequest = (int)el;
                    LOG("[drawlog] periodico t=%.1f: frames %lld y %lld (shot_%03d)", el, frame, frame + 1, (int)el);
                }
            }
        }
    }
    if (frame >= 0 && (long long)g_frameCounter > frame && (long long)g_frameCounter < frame + n) return (long long)g_frameCounter;
    return frame;
}

// ---- memexport (GoWJ sesion 4) ----
// El VS escribe registros (eA, k, datos) en g_memexBuf (contador en el byte 0). Tras el draw se copian a la CPU y se aplican a la
// memoria del invitado (MemexApply). De momento se espera a la GPU en cada draw con memexport (correcto pero lento).
static ComPtr<ID3D12Resource> g_memexBuf, g_memexRb;
static std::vector<std::pair<uint32_t, uint64_t>> g_memexVerify;   // (direccion, 8 bytes esperados)

static constexpr UINT kMemexRecs = 262144;   // GoWJ sesion 11: antes 87000 (copias de UE3 de hasta 117.760 registros se cortaban)
static constexpr UINT kMemexBytes = 16 + kMemexRecs * 48;
static void MemexBind(bool reset)
{
    if (!g_memexBuf)
    {
        D3D12_HEAP_PROPERTIES def{ D3D12_HEAP_TYPE_DEFAULT }, rb{ D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = kMemexBytes; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        g_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&g_memexBuf));
        bd.Flags = D3D12_RESOURCE_FLAG_NONE;
        g_dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_memexRb));
        LOG("[memex] bufer de registros creado (%u bytes)", kMemexBytes);
    }
    if (reset)
    {
        UINT64 o = RingAlloc(16, 16);
        if (o != ~0ull)
        {
            memset(g_ringMap[g_fi] + o, 0, 16);
            auto b1 = Transition(g_memexBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST); ProbedBarrier(1, &b1, __LINE__);
            g_list->CopyBufferRegion(g_memexBuf.Get(), 0, g_ring[g_fi].Get(), o, 16);
            auto b2 = Transition(g_memexBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); ProbedBarrier(1, &b2, __LINE__);
        }
    }
    g_list->SetGraphicsRootUnorderedAccessView(4, g_memexBuf->GetGPUVirtualAddress());
}
static void MemexApply(const uint8_t* rec, uint32_t n, uint64_t vsHash);
static void MemexProcess(uint64_t vsHash, uint32_t boundRecs = kMemexRecs)
{
    // GoWJ sesion 11: solo se copia la parte usada (cota de registros de los draws del lote); antes 4 MB por cada espera
    const uint32_t copyRecs = std::min<uint32_t>(std::max<uint32_t>(boundRecs, 4096u), kMemexRecs);
    if (g_dbgMemexPending) { g_dbgMemexPending = 0; static int nl = 0; if (nl++ < 10) LOG("[memexdbg] MemexProcess alcanzado"); }
    auto b1 = Transition(g_memexBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE); ProbedBarrier(1, &b1, __LINE__);
    g_list->CopyBufferRegion(g_memexRb.Get(), 0, g_memexBuf.Get(), 0, 16ull + uint64_t(copyRecs) * 48);
    auto b2 = Transition(g_memexBuf.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); ProbedBarrier(1, &b2, __LINE__);
    FlushAndWait();
    uint8_t* p = nullptr; g_memexRb->Map(0, nullptr, (void**)&p);
    if (!p) return;
    if (*(const uint32_t*)p > copyRecs) { static int no = 0; if (no++ < 10) LOG("[memex] registros perdidos: %u escritos, %u leidos (max %u), VS %016llX", *(const uint32_t*)p, copyRecs, kMemexRecs, (unsigned long long)vsHash); }
    uint32_t n = std::min<uint32_t>(*(const uint32_t*)p, copyRecs);
    MemexApply(p + 16, n, vsHash);
    D3D12_RANGE none{ 0, 0 }; g_memexRb->Unmap(0, &none);
    if (!g_memexVerify.empty())
    {   // GOWJ_MEMEXVERIFY: compara la copia de la CPU con lo que escribio la GPU
        uint32_t diff = 0; for (auto& pv : g_memexVerify) { uint64_t m; memcpy(&m, g_base + 0xA0000000u + pv.first, 8); if (m != pv.second) diff++; }
        static int nv = 0; if (nv++ < 30 || diff) { static int nd2 = 0; if (nv <= 30 || nd2++ < 30) LOG("[memexcpu] verificacion: %zu elementos, %u distintos", g_memexVerify.size(), diff); }
        g_memexVerify.clear();
    }
    // GoWJ sesion 11: tras FlushAndWait la GPU ha terminado todo lo grabado: el anillo y los descriptores del frame se pueden reutilizar.
    // UE3 mueve texturas con decenas de draws de copia por memexport que escriben encima de su origen (cada draw invalida la copia del
    // bufer de vertices de 256 KB): el anillo de 128 MB se llenaba, los draws siguientes se omitian y la copia quedaba a medias (texto,
    // HUD, armas y armaduras rotos). GOWJ_NORINGRECYCLE=1 = antes.
    static const bool noRecycle = getenv("GOWJ_NORINGRECYCLE") != nullptr;
    if (!noRecycle && g_ringOffset > kRingSize / 2)
    {
        g_ringOffset = 0; g_vbCache.clear(); g_descCursor = 0;
        static int nl = 0; if (nl++ < 8) LOG("[memex] anillo reciclado tras una copia por memexport (frame %llu)", (unsigned long long)g_frameCounter);
    }
}
// GoWJ sesion 11 (tirones): UE3 mueve texturas con rafagas de draws de copia por memexport (100+ en un cuadro) y cada uno esperaba
// a la GPU (vaciado de ~1 ms): tirones de 50-130 ms. Como cada registro lleva su direccion, se acumulan los de draws de memexport
// seguidos en el mismo bufer y se aplican con una sola espera antes del primer draw normal, de un resolve o de presentar (nadie lee
// la memoria a medias). GOWJ_MEMEXSYNC=1 = una espera por draw (antes).
static bool g_memexPending = false; static uint32_t g_memexPendBound = 0, g_memexPendDraws = 0; static uint64_t g_memexPendVs = 0;
void MemexFlushPending()
{
    if (!g_memexPending) return;
    g_memexPending = false; const uint32_t bound = g_memexPendBound; g_memexPendBound = 0;
    static uint64_t batches = 0, draws = 0; batches++; draws += g_memexPendDraws; g_memexPendDraws = 0;
    if ((batches & (batches - 1)) == 0) LOG("[memex] lotes %llu, draws de memexport %llu (%.1f por espera)", (unsigned long long)batches, (unsigned long long)draws, double(draws) / double(batches));
    MemexProcess(g_memexPendVs, bound);
}
static uint64_t g_mxWin = 0, g_mxLo = ~0ull, g_mxHi = 0;   // diagnostico GOWJ_TDWATCH (sesion 11)
static void MemexApply(const uint8_t* rec, uint32_t n, uint64_t vsHash)
{
    g_mxWin = 0; g_mxLo = ~0ull; g_mxHi = 0;
    // eA (constante de stream de memexport, deducida de los datos de GoWJ y compatible con Xenia):
    //   x = 0x40000000 | direccion base en dwords; y = 0x4B000000 + indice de elemento; z = 0x4B0xxxxx: endian (bits 0-2),
    //   formato de vertice (bits 8-13), tipo numerico (bits 16-18: 0 unorm, 1 snorm, 2 uint, 3 sint, 7 float); w = 0x4B000000 + elementos.
    static const bool logRecs = getenv("GOWJ_MEMEXLOG") != nullptr;
    static int logged = 0, unk = 0;
    uint32_t written = 0, skipped = 0;
    for (uint32_t i = 0; i < n; i++)
    {
        const uint32_t* r = (const uint32_t*)(rec + size_t(i) * 48); const float* v = (const float*)(rec + size_t(i) * 48 + 32);
        uint32_t baseDw = r[0] & 0x3FFFFFFF, index = (r[1] & 0x7FFFFF) + r[4], fmt = (r[2] >> 8) & 0x3F, endian = r[2] & 7, numf = (r[2] >> 16) & 7;
        uint32_t count = r[3] & 0x7FFFFF;
        if ((r[1] >> 23) != 0x96 || (r[2] >> 20) != 0x4B0) { skipped++; continue; }   // 0x4B0 = 2^23 (indices enteros)
        if (count && index >= count) { skipped++; continue; }
        auto conv = [&](float x, uint32_t bits) -> uint32_t {
            float mx = float((1u << bits) - 1), hm = float((1u << (bits - 1)) - 1);
            if (std::isnan(x)) x = 0;
            switch (numf)
            {
            case 0: return (uint32_t)(std::clamp(x, 0.0f, 1.0f) * mx + 0.5f);
            case 1: return (uint32_t)(int32_t)std::lround(std::clamp(x, -1.0f, 1.0f) * hm) & (uint32_t)mx;
            case 2: return (uint32_t)std::clamp(x, 0.0f, mx);
            case 3: return (uint32_t)(int32_t)std::clamp(x, -hm - 1, hm) & (uint32_t)mx;
            default: return (uint32_t)std::clamp(x, 0.0f, mx);
            }
        };
        uint32_t w[4] = {}; uint32_t bytes = 0;
        switch (fmt)
        {
        case 6:  w[0] = conv(v[0], 8) | (conv(v[1], 8) << 8) | (conv(v[2], 8) << 16) | (conv(v[3], 8) << 24); bytes = 4; break;   // 8_8_8_8
        case 7:  w[0] = conv(v[0], 10) | (conv(v[1], 10) << 10) | (conv(v[2], 10) << 20) | (conv(v[3], 2) << 30); bytes = 4; break; // 2_10_10_10
        case 25: w[0] = conv(v[0], 16) | (conv(v[1], 16) << 16); bytes = 4; break;                                                 // 16_16
        case 26: w[0] = conv(v[0], 16) | (conv(v[1], 16) << 16); w[1] = conv(v[2], 16) | (conv(v[3], 16) << 16); bytes = 8; break; // 16_16_16_16
        case 31: w[0] = FloatToHalf(v[0]) | (uint32_t(FloatToHalf(v[1])) << 16); bytes = 4; break;                                 // 16_16_FLOAT
        case 32: w[0] = FloatToHalf(v[0]) | (uint32_t(FloatToHalf(v[1])) << 16); w[1] = FloatToHalf(v[2]) | (uint32_t(FloatToHalf(v[3])) << 16); bytes = 8; break;
        case 33: case 36: memcpy(&w[0], &v[0], 4); if (fmt == 33) w[0] = conv(v[0], 31); bytes = 4; break;                          // 32 / 32_FLOAT
        case 34: case 37: memcpy(w, v, 8); bytes = 8; break;                                                                       // 32_32(_FLOAT)
        case 35: case 38: memcpy(w, v, 16); bytes = 16; break;                                                                     // 32_32_32_32(_FLOAT)
        default: if (unk++ < 8) LOG("[memex] formato %u sin soporte (eA %08X %08X %08X %08X)", fmt, r[0], r[1], r[2], r[3]); skipped++; continue;
        }
        uint64_t addr = uint64_t(baseDw) * 4 + uint64_t(index) * bytes;
        if (addr + bytes > 0x20000000) { skipped++; continue; }
        uint8_t* out = g_base + 0xA0000000u + (uint32_t)addr;
        {   // diagnostico (GoWJ sesion 11): GOWJ_TDWATCH
            static uint32_t lo = 0, hi = 0; static bool init = [] { if (const char* s = getenv("GOWJ_TDWATCH")) { char* e; lo = strtoul(s, &e, 16) & 0x1FFFFFFF; hi = lo + (*e == ',' ? strtoul(e + 1, nullptr, 16) : 0x1000); } return true; }(); (void)init;
            if (hi && addr >= lo && addr < hi) { g_mxWin++; }
            g_mxLo = std::min<uint64_t>(g_mxLo, addr); g_mxHi = std::max<uint64_t>(g_mxHi, addr + bytes);
        }
        for (uint32_t k = 0; k < bytes / 4; k++) { uint32_t s = Swap32(w[k], endian); memcpy(out + k * 4, &s, 4); }
        written++;
    }
    if (written) { g_cpuResolveCounter++; }   // memoria cambiada: invalida las huellas de textura memorizadas
    static const uint64_t mxh = getenv("GOWJ_MEMEXLOG") ? strtoull(getenv("GOWJ_MEMEXLOG"), nullptr, 16) : 0;
    const uint32_t c6d = (Reg(0x4000 + 6 * 4) & 0x3FFFFFFF) * 4;
    if (g_mxWin || (mxh == vsHash && c6d >= 0x01DF0000u && c6d < 0x01E40000u)) { static int nm = 0; if (nm++ < 200) LOG("[tdwatch] memexport VS %016llX frame %llu: %u registros (%u escritos, %u omitidos), %llu en la ventana; escrito %08llX..%08llX | eA del shader %08X %08X, c6 del registro %08X, vf0 %08X %08X", (unsigned long long)vsHash, (unsigned long long)g_frameCounter, n, written, skipped, (unsigned long long)g_mxWin, (unsigned long long)g_mxLo, (unsigned long long)g_mxHi, n ? ((const uint32_t*)rec)[0] : 0, n ? ((const uint32_t*)rec)[1] : 0, Reg(0x4000 + 6 * 4), Reg(0x4800), Reg(0x4801)); }
    {   // diagnostico (GoWJ sesion 11): por VS, registros omitidos (eA desconocido) y rango escrito
        static std::unordered_map<uint64_t, int> seenVs;
        if (seenVs.size() < 64 && seenVs[vsHash]++ == 0 && n)
        {
            const uint32_t* r = (const uint32_t*)rec;
            LOG("[memex] VS %016llX (primera vez, frame %llu): %u registros, %u escritos, %u omitidos; eA %08X %08X %08X %08X datos %g %g %g %g", (unsigned long long)vsHash, (unsigned long long)g_frameCounter, n, written, skipped,
                r[0], r[1], r[2], r[3], ((const float*)rec)[8], ((const float*)rec)[9], ((const float*)rec)[10], ((const float*)rec)[11]);
        }
    }
    if (logRecs && logged < 20)
    {
        logged++;
        const uint32_t* r = (const uint32_t*)rec;
        LOG("[memex] VS %016llX: %u registros, %u escritos, %u omitidos (eA %08X %08X %08X %08X)", (unsigned long long)vsHash, n, written, skipped,
            n ? r[0] : 0, n ? r[1] : 0, n ? r[2] : 0, n ? r[3] : 0);
    }
}
static void DumpEdram(UINT64 frame);static void D3D12DrawImpl(const XenosDraw& d);
// Envoltorio (sesion 21): en el frame de GOWJ_DRAWLOG indica para cada draw si se dibujo o por que se omitio
void D3D12Draw(const XenosDraw& d)
{
    {
        if (g_dbgMemexPending) { g_dbgMemexPending = 0; static int nl = 0; if (nl++ < 20) { std::string m; for (int i = 0; i < 16; i++) if (g_skipped[i] != g_dbgSkipSnap[i]) m += " " + std::to_string(i); LOG("[memexdbg] draw de copia NO llego a MemexProcess; contadores omitidos que cambiaron:%s", m.c_str()); } } }
    struct DrawTimer { double t0 = QpcMs(); ~DrawTimer() { g_hDrawMs += QpcMs() - t0; } } dtimer;
    g_curPsHash = d.psHash;
    {   // GOWJ_VSCLOG=<hash VS>:c,c,... (diagnostico, sesion 10): esas constantes del VS en los 6 primeros draws con ese VS
        static uint64_t vh = 0; static std::vector<int> vcs = [] { std::vector<int> v; if (const char* e = getenv("GOWJ_VSCLOG")) { vh = strtoull(e, nullptr, 16); const char* c = strchr(e, ':'); while (c) { v.push_back(atoi(c + 1)); c = strchr(c + 1, ','); } } return v; }();
        static int vl = 0;
        if (vh && d.vsHash == vh && vl < 6)
        {
            vl++; std::string line; char b[96];
            for (int c : vcs) { snprintf(b, sizeof(b), " c%d=(%g %g %g %g)", c, RegF(0x4000 + c * 4), RegF(0x4000 + c * 4 + 1), RegF(0x4000 + c * 4 + 2), RegF(0x4000 + c * 4 + 3)); line += b; }
            LOG("[vsconst] VS %016llX PS %016llX f%llu:%s", (unsigned long long)d.vsHash, (unsigned long long)d.psHash, (unsigned long long)g_frameCounter, line.c_str());
        }
    }
    {   // GOWJ_PSFULLLOG=<hash PS> (diagnostico, sesion 10): en el cuadro de GOWJ_DRAWLOG, constantes c0-c31 y c240-c255 del PS y los
        // 256 booleanos (0x4900-0x4907) de cada draw con ese PS
        static const uint64_t fh = getenv("GOWJ_PSFULLLOG") ? strtoull(getenv("GOWJ_PSFULLLOG"), nullptr, 16) : 0;
        if (fh && d.psHash == fh && (long long)g_frameCounter == DrawLogFrame())
        {
            std::string cs; char b[96];
            for (int c = 0; c < 256; c++)
            {
                if (c == 32) c = 240;
                float f[4]; for (int k = 0; k < 4; k++) { uint32_t u = Reg(0x4400 + c * 4 + k); memcpy(&f[k], &u, 4); }
                if (f[0] == 0 && f[1] == 0 && f[2] == 0 && f[3] == 0) continue;
                snprintf(b, sizeof(b), " c%d=(%.4g %.4g %.4g %.4g)", c, f[0], f[1], f[2], f[3]); cs += b;
            }
            std::string bs; for (int i = 0; i < 8; i++) { snprintf(b, sizeof(b), " %08X", Reg(0x4900 + i)); bs += b; }
            LOG("[psfull] f%llu draws %llu PS %016llX VS %016llX bools%s |%s", (unsigned long long)g_frameCounter, (unsigned long long)g_frameDraws, (unsigned long long)d.psHash, (unsigned long long)d.vsHash, bs.c_str(), cs.c_str());
        }
    }
    {   // GOWJ_PSCONSTLOG=<hash PS>[,<hash>...]: constantes PS c0-c15, mezcla y mascara de color de los draws con ese PS (diagnostico)
        static std::vector<uint64_t> hs = [] { std::vector<uint64_t> v; const char* s = getenv("GOWJ_PSCONSTLOG"); while (s && *s) { v.push_back(strtoull(s, nullptr, 16)); s = strchr(s, ','); if (s) s++; } return v; }();
        static int logged = 0;
        if (!hs.empty() && logged < 60 && std::find(hs.begin(), hs.end(), d.psHash) != hs.end())
        {
            logged++;
            std::string cs;
            static const int extra[] = { 20, 110, 111, 152, 153 };
            for (int ci = 0; ci < 16 + 5; ci++)
            {
                int c = ci < 16 ? ci : extra[ci - 16];
                char b[96]; float f[4];
                for (int k = 0; k < 4; k++) { uint32_t u = Reg(0x4400 + c * 4 + k); memcpy(&f[k], &u, 4); }
                snprintf(b, sizeof(b), " c%d=(%.4g %.4g %.4g %.4g)", c, f[0], f[1], f[2], f[3]); cs += b;
            }
            for (int c = 0; c < 4; c++)
            {
                char b[96]; float f[4];
                for (int k = 0; k < 4; k++) { uint32_t u = Reg(0x4000 + c * 4 + k); memcpy(&f[k], &u, 4); }
                snprintf(b, sizeof(b), " vc%d=(%.4g %.4g %.4g %.4g)", c, f[0], f[1], f[2], f[3]); cs += b;
            }
            LOG("[psconst] PS %016llX VS %016llX frame %llu blend0=%08X mask=%08X color0=%08X depthctl=%08X |%s", (unsigned long long)d.psHash, (unsigned long long)d.vsHash,
                (unsigned long long)g_frameCounter, Reg(0x2201), Reg(0x2104), Reg(0x2001), Reg(0x2200), cs.c_str());
        }
    }
    {   // GOWJ_DUMPVS: la primera vez que aparece ese VS se registra el frame en curso desde ese draw (sesion 24)
        static uint64_t vsTrig = getenv("GOWJ_DUMPVS") ? strtoull(getenv("GOWJ_DUMPVS"), nullptr, 16) : 0;
        static bool trig = false;
        static DWORD vsT0 = GetTickCount(); static double vsMin = getenv("GOWJ_DUMPVSMIN") ? atof(getenv("GOWJ_DUMPVSMIN")) : 0;
        if (vsTrig && !trig && d.vsHash == vsTrig && (GetTickCount() - vsT0) / 1000.0 >= vsMin) { trig = true; g_forceLogFrame = (long long)g_frameCounter; LOG("[drawlog] GOWJ_DUMPVS: frame %lld", g_forceLogFrame); }
    }
    {   // GOWJ_VSCONSTN=<n> (diagnostico): constantes del VS de los draws de n indices (matrices de huesos)
        static const uint32_t cn = getenv("GOWJ_VSCONSTN") ? atoi(getenv("GOWJ_VSCONSTN")) : 0;
        static const uint64_t cvs = getenv("GOWJ_VSCONSTVS") ? strtoull(getenv("GOWJ_VSCONSTVS"), nullptr, 16) : 0;
        static int logged = 0;
        {   // GOWJ_VBUSE=<dir fisica hex>: draws cuyo stream vf0 (slot 95) empieza ahi: VS, n y rango de indices usados (tras GOWJ_VSCONSTMIN)
            static const uint32_t vbu = getenv("GOWJ_VBUSE") ? (uint32_t)strtoul(getenv("GOWJ_VBUSE"), nullptr, 16) : 0;
            static const double umin = getenv("GOWJ_VSCONSTMIN") ? atof(getenv("GOWJ_VSCONSTMIN")) : 0;
            static const DWORD ut0 = [] { FILETIME c, e, k, u; GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u); FILETIME now; GetSystemTimeAsFileTime(&now); uint64_t a = (uint64_t(c.dwHighDateTime) << 32) | c.dwLowDateTime, b = (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime; return GetTickCount() - (DWORD)((b - a) / 10000); }();
            static int ulog = 0;
            if (vbu && (Reg(0x4800 + 95 * 2) & 0x1FFFFFFC) == vbu && d.source == 0 && d.indexAddr && ulog < 40 && (GetTickCount() - ut0) / 1000.0 >= umin)
            {
                ulog++; uint32_t lo = ~0u, hi = 0;
                for (uint32_t i = 0; i < d.numIndices; i++)
                {
                    uint32_t raw = d.index32 ? *(uint32_t*)(g_base + (0xA0000000u | d.indexAddr) + i * 4) : *(uint16_t*)(g_base + (0xA0000000u | d.indexAddr) + i * 2);
                    uint32_t idx = d.index32 ? __builtin_bswap32(raw) : __builtin_bswap16((uint16_t)raw);
                    if (!d.index32 && idx == 0xFFFF) continue;
                    idx += Reg(0x2102); lo = std::min(lo, idx); hi = std::max(hi, idx);
                }
                if (d.vsHash == 0xE7323601B67A98E3ull)
                {   // E7323601 en la CPU (desensamblado de Xenia): 4 huesos ubyte4 +12, pesos halves +36 (h0..h3): (1-h1-h2-h3, h1, h2, h3)
                    uint32_t ma = Reg(0x4800 + 94 * 2) & 0x1FFFFFFC;
                    auto F = [](uint32_t a) { uint32_t u = __builtin_bswap32(*(uint32_t*)(g_base + (0xA0000000u | a))); float f; memcpy(&f, &u, 4); return f; };
                    auto H = [](uint16_t h) { uint32_t e = (h >> 10) & 31; float r = e == 0 ? (h & 1023) / 1024.0f / 16384.0f : (1 + (h & 1023) / 1024.0f) * powf(2.0f, (float)e - 15); return (h & 0x8000) ? -r : r; };
                    float c[4][4]; for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) { uint32_t u = Reg(0x4000 + (29 + r) * 4 + k); memcpy(&c[r][k], &u, 4); }
                    float mn[5] = { 1e9f, 1e9f, 1e9f, 1e9f, 1e9f }, mx[5] = { -1e9f, -1e9f, -1e9f, -1e9f, -1e9f }; std::string ex;
                    for (uint32_t vi = lo; vi <= hi && vi != ~0u; vi++)
                    {
                        uint32_t v = vbu + vi * 44; float pp[4] = { F(v), F(v + 4), F(v + 8), 1 };
                        uint32_t bi = __builtin_bswap32(*(uint32_t*)(g_base + (0xA0000000u | (v + 12))));
                        uint32_t w0 = __builtin_bswap32(*(uint32_t*)(g_base + (0xA0000000u | (v + 36)))), w1 = __builtin_bswap32(*(uint32_t*)(g_base + (0xA0000000u | (v + 40))));
                        float h[4] = { H((uint16_t)w0), H((uint16_t)(w0 >> 16)), H((uint16_t)w1), H((uint16_t)(w1 >> 16)) };
                        float wt[4] = { 1 - h[1] - h[2] - h[3], h[1], h[2], h[3] }; uint32_t b[4] = { bi & 0xFF, (bi >> 8) & 0xFF, (bi >> 16) & 0xFF, bi >> 24 };
                        float W[3] = {};
                        for (int j = 0; j < 4; j++) for (int r = 0; r < 3; r++) { float a = 0; for (int k = 0; k < 4; k++) a += pp[k] * F(ma + b[j] * 48 + r * 16 + k * 4); W[r] += wt[j] * a; }
                        float o[4]; for (int k = 0; k < 4; k++) o[k] = W[0] * c[0][k] + W[1] * c[1][k] + W[2] * c[2][k] + c[3][k];
                        float q[5] = { W[0], W[1], W[2], o[0] / o[3], o[1] / o[3] };
                        for (int k = 0; k < 5; k++) { mn[k] = std::min(mn[k], q[k]); mx[k] = std::max(mx[k], q[k]); }
                        if ((vi - lo) % 400 == 0) { char t[200]; snprintf(t, sizeof(t), " | v%u huesos %u,%u,%u,%u pesos %.2f,%.2f,%.2f,%.2f ndc(%.2f %.2f)", vi, b[0], b[1], b[2], b[3], wt[0], wt[1], wt[2], wt[3], q[3], q[4]); ex += t; }
                    }
                    LOG("[vbuse]   E73 en CPU: mundo x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f, pantalla x %.2f..%.2f y %.2f..%.2f%s", mn[0], mx[0], mn[1], mx[1], mn[2], mx[2], mn[3], mx[3], mn[4], mx[4], ex.c_str());
                }
                LOG("[vbuse] frame %llu VS %016llX PS %016llX n %u: vertices %u..%u, mascara %X blend %08X", (unsigned long long)g_frameCounter, (unsigned long long)d.vsHash, (unsigned long long)d.psHash, d.numIndices, lo, hi, Reg(0x2104) & 0xF, Reg(0x2201));
            }
        }
        {   // GOWJ_VBRANGE=<lo hex>:<hi hex> (sesion 24): draws con algun stream de vertices en ese rango fisico (efectos de effpk_common)
            static uint32_t rlo = 0, rhi = 0; static bool rinit = false;
            if (!rinit) { rinit = true; if (const char* e = getenv("GOWJ_VBRANGE")) { char* q; rlo = (uint32_t)strtoul(e, &q, 16); if (*q == ':') rhi = (uint32_t)strtoul(q + 1, nullptr, 16); } }
            static std::atomic<int> rlog{ 0 };
            // resumen cada 600 frames: draws por (VS,PS) con vertices en el rango (sin limite)
            static std::map<std::pair<uint64_t, uint64_t>, uint32_t> rsum; static uint64_t rlast = 0;
            if (rhi)
            {
                for (uint32_t slot = 88; slot < 96; slot++) { uint32_t a = Reg(0x4800 + slot * 2) & 0x1FFFFFFC; if (a >= rlo && a < rhi) { rsum[{ d.vsHash, d.psHash }]++; break; } }
                if (g_frameCounter >= rlast + 600)
                {
                    std::string m; for (auto& kv : rsum) { char t[64]; snprintf(t, sizeof(t), " %08X/%08X=%u", (uint32_t)(kv.first.first >> 32), (uint32_t)(kv.first.second >> 32), kv.second); m += t; }
                    LOG("[vbrsum] frames %llu..%llu:%s", (unsigned long long)rlast, (unsigned long long)g_frameCounter, m.c_str());
                    rsum.clear(); rlast = g_frameCounter;
                }
            }
            if (rhi && rlog < 200)
                for (uint32_t slot = 88; slot < 96; slot++)
                {
                    uint32_t a = Reg(0x4800 + slot * 2) & 0x1FFFFFFC;
                    if (a >= rlo && a < rhi)
                    {
                        rlog++;
                        LOG("[vbrange] frame %llu slot %u @%08X VS %016llX PS %016llX prim %u n %u mascara %X blend %08X dc %08X surface %08X color0 %08X modo %u",
                            (unsigned long long)g_frameCounter, slot, a, (unsigned long long)d.vsHash, (unsigned long long)d.psHash, d.primType, d.numIndices,
                            Reg(0x2104) & 0xF, Reg(0x2201), Reg(0x2200), Reg(0x2000), Reg(0x2001), Reg(0x2208) & 7);
                        break;
                    }
                }
        }
        if (cvs && d.vsHash == cvs && logged < 16)
        {
            logged++;
            auto C = [](int c) { float f[4]; for (int k = 0; k < 4; k++) { uint32_t u = Reg(0x4000 + c * 4 + k); memcpy(&f[k], &u, 4); } char b[96]; snprintf(b, sizeof(b), "c%d=(%.3g %.3g %.3g %.3g) ", c, f[0], f[1], f[2], f[3]); return std::string(b); };
            std::string m = C(49) + C(200) + C(201) + C(255);
            for (int vf = 0; vf < 6; vf++)
            {
                uint32_t slot = 95 - vf, a = Reg(0x4800 + slot * 2) & 0x1FFFFFFC, b = ((Reg(0x4800 + slot * 2 + 1) >> 2) & 0xFFFFFF) * 4;
                uint32_t bad = 0, big = 0; float mx = 0;
                if (a && b && b < (8u << 20)) for (uint32_t i = 0; i < b / 4; i++) { uint32_t u = __builtin_bswap32(*(uint32_t*)(g_base + (0xA0000000u | a) + i * 4)); float f; memcpy(&f, &u, 4); if (f != f || std::isinf(f)) bad++; else { if (fabsf(f) > 1e4f) big++; mx = std::max(mx, fabsf(f)); } }
                char t[128]; snprintf(t, sizeof(t), "| vf%d @%08X %u B NaN %u >1e4 %u max %g ", vf, a, b, bad, big, mx); m += t;
            }
            LOG("[vsconstvs] VS %016llX n=%u frame %llu: %s", (unsigned long long)d.vsHash, d.numIndices, (unsigned long long)g_frameCounter, m.c_str());
        }
        static const double cmin = getenv("GOWJ_VSCONSTMIN") ? atof(getenv("GOWJ_VSCONSTMIN")) : 0;
        static const DWORD ct0 = [] { FILETIME c, e, k, u; GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u); FILETIME now; GetSystemTimeAsFileTime(&now); uint64_t a = (uint64_t(c.dwHighDateTime) << 32) | c.dwLowDateTime, b = (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime; return GetTickCount() - (DWORD)((b - a) / 10000); }();
        if (cn && d.numIndices == cn && logged < 6 && (GetTickCount() - ct0) / 1000.0 >= cmin)
        {
            logged++;
            int nan = 0, zero = 0, huge = 0;
            std::string cs;
            for (int c = 0; c < 256; c++)
            {
                float f[4]; for (int k = 0; k < 4; k++) { uint32_t u = Reg(0x4000 + c * 4 + k); memcpy(&f[k], &u, 4); if (f[k] != f[k] || std::isinf(f[k])) nan++; else if (fabsf(f[k]) > 1e6f) huge++; }
                if (f[0] == 0 && f[1] == 0 && f[2] == 0 && f[3] == 0) zero++;
                if (c < 96) { char b[80]; snprintf(b, sizeof(b), " c%d=(%.3g %.3g %.3g %.3g)", c, f[0], f[1], f[2], f[3]); cs += b; }
            }
            LOG("[vsconst] VS %016llX PS %016llX frame %llu: NaN/Inf %d, enormes %d, vectores cero %d |%s", (unsigned long long)d.vsHash, (unsigned long long)d.psHash,
                (unsigned long long)g_frameCounter, nan, huge, zero, cs.c_str());
            if (d.vsHash == 0xF80101051A52517Cull && d.source == 0 && d.indexAddr)
            {   // VS F8010105 en la CPU (como el desensamblado de Xenia): dos huesos (ubyte4 +12: x = A, y = B), peso = half alto de +36,
                // mundo = lerp(A*p, B*p, peso), clip = X*c29 + Y*c30 + Z*c31 + c32
                uint32_t va = Reg(0x4800 + 95 * 2) & 0x1FFFFFFC, ma = Reg(0x4800 + 94 * 2) & 0x1FFFFFFC, ioff = Reg(0x2102);
                auto F = [](uint32_t a) { uint32_t u = __builtin_bswap32(*(uint32_t*)(g_base + (0xA0000000u | a))); float f; memcpy(&f, &u, 4); return f; };
                float c[4][4]; for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) { uint32_t u = Reg(0x4000 + (29 + r) * 4 + k); memcpy(&c[r][k], &u, 4); }
                std::string m;
                for (uint32_t i = 0; i < d.numIndices && i < 4453; i += d.numIndices / 10 + 1)
                {
                    uint32_t raw = d.index32 ? *(uint32_t*)(g_base + (0xA0000000u | d.indexAddr) + i * 4) : *(uint16_t*)(g_base + (0xA0000000u | d.indexAddr) + i * 2);
                    uint32_t idx = d.index32 ? __builtin_bswap32(raw) : __builtin_bswap16((uint16_t)raw);
                    if (!d.index32 && idx == 0xFFFF) continue;
                    idx += ioff;
                    uint32_t v = va + idx * 44;
                    float pp[4] = { F(v), F(v + 4), F(v + 8), 1 };
                    uint32_t bi = __builtin_bswap32(*(uint32_t*)(g_base + (0xA0000000u | (v + 12))));
                    uint32_t bA = bi & 0xFF, bB = (bi >> 8) & 0xFF;
                    uint32_t wd = __builtin_bswap32(*(uint32_t*)(g_base + (0xA0000000u | (v + 36))));
                    auto h2f = [](uint16_t h) { uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, f = h & 1023; float r = e == 0 ? f / 1024.0f / 16384.0f : (e == 31 ? INFINITY : (1 + f / 1024.0f) * powf(2.0f, (float)e - 15)); return s ? -r : r; };
                    float w = h2f((uint16_t)(wd >> 16));
                    float W[3];
                    for (int r = 0; r < 3; r++)
                    {
                        float a = 0, b = 0; for (int k = 0; k < 4; k++) { a += pp[k] * F(ma + bA * 48 + r * 16 + k * 4); b += pp[k] * F(ma + bB * 48 + r * 16 + k * 4); }
                        W[r] = a + (b - a) * w;
                    }
                    float o[4]; for (int k = 0; k < 4; k++) o[k] = W[0] * c[0][k] + W[1] * c[1][k] + W[2] * c[2][k] + c[3][k];
                    char b[200]; snprintf(b, sizeof(b), " | v%u p(%.3g %.3g %.3g) huesos %u/%u peso %.3g mundo(%.3g %.3g %.3g) ndc(%.2f %.2f) w %.2f", idx, pp[0], pp[1], pp[2], bA, bB, w, W[0], W[1], W[2], o[0] / o[3], o[1] / o[3], o[3]);
                    m += b;
                }
                LOG("[vsconst]   F80 en CPU (indices @%08X, offset %u):%s", d.indexAddr, ioff, m.c_str());
                {   // todos los vertices: por hueso A, numero, caja del modelo y caja en pantalla
                    struct Box { uint32_t n = 0; float mn[5] = { 1e9f, 1e9f, 1e9f, 1e9f, 1e9f }, mx[5] = { -1e9f, -1e9f, -1e9f, -1e9f, -1e9f }; };
                    std::map<uint32_t, Box> boxes;
                    for (uint32_t i = 0; i < d.numIndices; i++)
                    {
                        uint32_t raw = d.index32 ? *(uint32_t*)(g_base + (0xA0000000u | d.indexAddr) + i * 4) : *(uint16_t*)(g_base + (0xA0000000u | d.indexAddr) + i * 2);
                        uint32_t idx = d.index32 ? __builtin_bswap32(raw) : __builtin_bswap16((uint16_t)raw);
                        if (!d.index32 && idx == 0xFFFF) continue;
                        uint32_t v = va + (idx + ioff) * 44;
                        float pp[4] = { F(v), F(v + 4), F(v + 8), 1 };
                        uint32_t bi = __builtin_bswap32(*(uint32_t*)(g_base + (0xA0000000u | (v + 12))));
                        uint32_t bA = bi & 0xFF, bB = (bi >> 8) & 0xFF;
                        uint32_t wd = __builtin_bswap32(*(uint32_t*)(g_base + (0xA0000000u | (v + 36))));
                        uint16_t hh = (uint16_t)(wd >> 16); float w = ((hh >> 10) & 31) == 0 ? 0 : (1 + (hh & 1023) / 1024.0f) * powf(2.0f, (float)((hh >> 10) & 31) - 15);
                        float W[3];
                        for (int r = 0; r < 3; r++) { float a = 0, b = 0; for (int k = 0; k < 4; k++) { a += pp[k] * F(ma + bA * 48 + r * 16 + k * 4); b += pp[k] * F(ma + bB * 48 + r * 16 + k * 4); } W[r] = a + (b - a) * w; }
                        float o[4]; for (int k = 0; k < 4; k++) o[k] = W[0] * c[0][k] + W[1] * c[1][k] + W[2] * c[2][k] + c[3][k];
                        float q[5] = { pp[0], pp[1], pp[2], o[0] / o[3], o[1] / o[3] };
                        Box& bx = boxes[bA * 256 + bB]; bx.n++;
                        for (int k = 0; k < 5; k++) { bx.mn[k] = std::min(bx.mn[k], q[k]); bx.mx[k] = std::max(bx.mx[k], q[k]); }
                    }
                    {   // todo el bufer de vertices (no solo los de este draw): caja del modelo por grupo de huesos
                        uint32_t vb = ((Reg(0x4800 + 95 * 2 + 1) >> 2) & 0xFFFFFF) * 4;
                        float g[3][6]; uint32_t gn[3] = {}; uint32_t gfirst[3] = { ~0u, ~0u, ~0u }, glast[3] = {};
                        for (auto& r : g) { r[0] = r[1] = r[2] = 1e9f; r[3] = r[4] = r[5] = -1e9f; }
                        for (uint32_t vi = 0; vi < vb / 44; vi++)
                        {
                            uint32_t v = va + vi * 44; uint32_t bi = __builtin_bswap32(*(uint32_t*)(g_base + (0xA0000000u | (v + 12))));
                            uint32_t bA = bi & 0xFF; int gi = bA == 4 ? 0 : (bA >= 22 && bA <= 78) ? 1 : 2;
                            float q[3] = { F(v), F(v + 4), F(v + 8) }; gn[gi]++; gfirst[gi] = std::min(gfirst[gi], vi); glast[gi] = vi;
                            for (int k = 0; k < 3; k++) { g[gi][k] = std::min(g[gi][k], q[k]); g[gi][k + 3] = std::max(g[gi][k + 3], q[k]); }
                        }
                        static const char* nm[3] = { "hueso 4", "huesos 22-78", "resto" };
                        for (int gi = 0; gi < 3; gi++) LOG("[vsconst]     bufer entero, %s: %u vertices (%u..%u), x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f", nm[gi], gn[gi], gfirst[gi], glast[gi], g[gi][0], g[gi][3], g[gi][1], g[gi][4], g[gi][2], g[gi][5]);
                    }
                    for (auto& [key, bx] : boxes)
                        LOG("[vsconst]     huesos %u/%u: %u indices, modelo x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f, pantalla x %.2f..%.2f y %.2f..%.2f", key >> 8, key & 255, bx.n,
                            bx.mn[0], bx.mx[0], bx.mn[1], bx.mx[1], bx.mn[2], bx.mx[2], bx.mn[3], bx.mx[3], bx.mn[4], bx.mx[4]);
                }
            }
            {   // indices de hueso de los vertices (slot 95, paso 44, ubyte4 en +12) y si su matriz (slot 94, 48 bytes) esta vacia
                uint32_t va = Reg(0x4800 + 95 * 2) & 0x1FFFFFFC, vb = ((Reg(0x4800 + 95 * 2 + 1) >> 2) & 0xFFFFFF) * 4;
                uint32_t ma = Reg(0x4800 + 94 * 2) & 0x1FFFFFFC, mb = ((Reg(0x4800 + 94 * 2 + 1) >> 2) & 0xFFFFFF) * 4;
                if (va && ma && vb && vb < (8u << 20))
                {
                    uint32_t cnt[256] = {};
                    for (uint32_t v = 0; v < vb / 44; v++) { uint32_t p = __builtin_bswap32(*(uint32_t*)(g_base + (0xA0000000u | va) + v * 44 + 12)); cnt[p & 0xFF]++; cnt[(p >> 8) & 0xFF]++; }
                    std::string m;
                    for (int b = 0; b < 256; b++) if (cnt[b])
                    {
                        bool zero = true; float t[3] = {};
                        if ((uint32_t)b * 48 + 48 <= mb) for (int k = 0; k < 12; k++) { uint32_t u = __builtin_bswap32(*(uint32_t*)(g_base + (0xA0000000u | ma) + b * 48 + k * 4)); if (u) zero = false; if (k % 4 == 3) memcpy(&t[k / 4], &u, 4); }
                        char s[64]; snprintf(s, sizeof(s), " %d:%u%s(%.2g,%.2g,%.2g)", b, cnt[b], zero ? "VACIA" : "", t[0], t[1], t[2]); m += s;
                    }
                    LOG("[vsconst]   huesos (%u vertices, matrices @%08X %u bytes):%s", vb / 44, ma, mb, m.c_str());
                }
            }
            for (int s = 0; s < 3; s++)   // fetch constants de vertices 95, 94, 93 (slots habituales): direccion, tamano y resumen
            {
                uint32_t slot = 95 - s, dw0 = Reg(0x4800 + slot * 2), dw1 = Reg(0x4800 + slot * 2 + 1);
                uint32_t addr = dw0 & 0x1FFFFFFC, bytes = ((dw1 >> 2) & 0xFFFFFF) * 4;
                if (!addr || !bytes || bytes > (4u << 20)) continue;
                const uint32_t* w = (const uint32_t*)(g_base + (0xA0000000u | addr));
                uint32_t n = bytes / 4, z = 0, bad = 0; float mx = 0;
                for (uint32_t i = 0; i < n; i++) { uint32_t u = __builtin_bswap32(w[i]); float f; memcpy(&f, &u, 4); if (u == 0) z++; if (f != f || std::isinf(f)) bad++; else mx = std::max(mx, fabsf(f)); }
                std::string m;
                for (uint32_t i = 0; i < std::min<uint32_t>(n, 48); i++) { uint32_t u = __builtin_bswap32(w[i]); float f; memcpy(&f, &u, 4); char b[24]; snprintf(b, sizeof(b), " %.3g", f); m += b; }
                LOG("[vsconst]   slot %u @%08X %u bytes: ceros %u, NaN %u, max %g |%s", slot, addr, bytes, z, bad, mx, m.c_str());
            }
        }
    }
    long long logFrame = DrawLogFrame();
    if (logFrame < 0 || g_failed || !g_dev) { D3D12DrawImpl(d); return; }
    OpenFrame();
    if ((long long)g_frameCounter != logFrame) { D3D12DrawImpl(d); return; }
    uint64_t before[8]; memcpy(before, g_skipped, sizeof(before));
    uint64_t drawsBefore = g_frameDraws;
    D3D12DrawImpl(d);
    static const char* names[8] = { "prim", "shader", "pso", "mem", "mascara0", "descriptores", "?", "?" };
    std::string why;
    for (int i = 0; i < 8; i++) if (g_skipped[i] != before[i]) why += names[i];
    LOG("[drawres] %s", g_frameDraws != drawsBefore ? "dibujado" : why.empty() ? "omitido (otro motivo)" : ("omitido: " + why).c_str());
}

// GOWJ_PROBECALLS=1 (diagnostico, sesion 8): repite la llamada en una lista de prueba y la cierra; si falla, la registra
template <class F> void ProbeCall(F f, const char* what, int line)
{
    static const bool probe = getenv("GOWJ_PROBECALLS") != nullptr;
    if (!probe) return;
    static ComPtr<ID3D12CommandAllocator> pa;
    if (!pa) g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&pa));
    if (!pa) return;
    ComPtr<ID3D12GraphicsCommandList> pl; pa->Reset();
    if (FAILED(g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, pa.Get(), nullptr, IID_PPV_ARGS(&pl)))) return;
    f(pl.Get());
    HRESULT hr = pl->Close();
    static int logged = 0;
    if (FAILED(hr) && logged++ < 30) LOG("[probe] INVALIDA linea %d: %s (hr %08X)", line, what, (unsigned)hr);
}
#define PROBED(call) do { ProbeCall([&](ID3D12GraphicsCommandList* L_) { L_->call; }, #call, __LINE__); g_list->call; } while (0)

// GOWJ_PIXELPROBE=x,y[;x,y...] (diagnostico, sesion 10): en el cuadro de GOWJ_DRAWLOG (o DRAWLOGEVERY), tras cada draw sobre una
// superficie de pantalla (1280 de ancho), copia esos pixeles (coordenadas de 1280x720) y al acabar el cuadro registra los draws
// que los cambian: "[probe] draw N VS PS blend: p0 (r g b a) ...". Sirve para saber que draw pinta algo concreto.
namespace {
struct ProbeRec { uint32_t draw; uint64_t vs, ps; uint32_t blend, surf; DXGI_FORMAT fmt; int gs; };
int g_probeGsKind = 0;   // GoWJ: GS del ultimo draw (2/3 = interfaz comprimida)
std::vector<ProbeRec> g_probeRecs; ComPtr<ID3D12Resource> g_probeRb; long long g_probeFrame = -1; uint32_t g_probeDrawIdx = 0;
std::vector<std::pair<int, int>> ProbePoints()
{
    // GoWJ sesion 11: sin GOWJ_PIXELPROBE se miran la mira y su alrededor (apuntar a lo que sale mal y pulsar F9)
    std::vector<std::pair<int, int>> v; const char* e = getenv("GOWJ_PIXELPROBE"); if (!e) e = "640,360;600,360;680,360;640,330;640,390";
    while (*e) { int x = 0, y = 0; if (sscanf(e, "%d,%d", &x, &y) == 2) v.push_back({ x, y }); const char* n = strchr(e, ';'); if (!n) break; e = n + 1; }
    if (v.size() > 8) v.resize(8);
    return v;
}
void ProbeFlush()
{
    static const auto pts = ProbePoints();
    if (g_probeRecs.empty() || !g_probeRb) return;
    FlushAndWait();
    uint8_t* p = nullptr; D3D12_RANGE rr{ 0, g_probeRecs.size() * pts.size() * 256 }; g_probeRb->Map(0, &rr, (void**)&p);
    if (p)
    {
        std::vector<std::string> last(pts.size());
        for (size_t i = 0; i < g_probeRecs.size(); i++)
        {
            const ProbeRec& r = g_probeRecs[i]; std::string line; bool changed = false;
            for (size_t k = 0; k < pts.size(); k++)
            {
                const uint8_t* px = p + (i * pts.size() + k) * 256; char b[96]; float c[4] = {};
                if (r.fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) for (int j = 0; j < 4; j++) c[j] = HalfToFloat(((const uint16_t*)px)[j]);
                else if (r.fmt == DXGI_FORMAT_B8G8R8A8_UNORM) { c[0] = px[2] / 255.f; c[1] = px[1] / 255.f; c[2] = px[0] / 255.f; c[3] = px[3] / 255.f; }
                else if (r.fmt == DXGI_FORMAT_R16G16B16A16_SNORM) for (int j = 0; j < 4; j++) c[j] = ((const int16_t*)px)[j] / 32767.f * 32.f;
                else { uint32_t u; memcpy(&u, px, 4); c[0] = (u & 1023) / 1023.f; c[1] = ((u >> 10) & 1023) / 1023.f; c[2] = ((u >> 20) & 1023) / 1023.f; c[3] = (u >> 30) / 3.f; }
                snprintf(b, sizeof(b), " p%zu(%.3f %.3f %.3f %.3f)", k, c[0], c[1], c[2], c[3]);
                std::string key = std::to_string(r.surf) + b; if (key != last[k]) { changed = true; last[k] = key; }
                line += b;
            }
            if (changed) LOG("[probe] f%lld draw %u VS %016llX PS %016llX blend %08X surf %08X fmt %d gs %d:%s", g_probeFrame, r.draw, (unsigned long long)r.vs, (unsigned long long)r.ps, r.blend, r.surf, (int)r.fmt, r.gs, line.c_str());
        }
        g_probeRb->Unmap(0, nullptr);
    }
    g_probeRecs.clear();
}
void ProbeAfterDraw(EdramRt* rt, const XenosDraw& d)
{
    static const auto pts = ProbePoints();
    if (pts.empty()) return;
    long long lf = DrawLogFrame();
    if ((long long)g_frameCounter != lf) { if (!g_probeRecs.empty()) ProbeFlush(); return; }
    if (g_probeFrame != (long long)g_frameCounter) { if (!g_probeRecs.empty()) ProbeFlush(); g_probeFrame = (long long)g_frameCounter; g_probeDrawIdx = 0; }
    g_probeDrawIdx++;
    if (!rt || !rt->res || (Reg(0x2000) & 0x3FFF) != 1280) return;
    constexpr size_t kMax = 6000;
    if (g_probeRecs.size() >= kMax) return;
    if (!g_probeRb)
    {
        D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK }; D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = kMax * 8 * 256;
        bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_probeRb)))) return;
    }
    uint32_t msx, msy; MsaaScale(msx, msy);
    const UINT sxx = std::max<UINT>(1, rt->w / 1280);
    auto a = Transition(rt->res.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE); ProbedBarrier(1, &a, __LINE__);
    for (size_t k = 0; k < pts.size(); k++)
    {
        D3D12_TEXTURE_COPY_LOCATION dst{ g_probeRb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        dst.PlacedFootprint.Offset = (g_probeRecs.size() * pts.size() + k) * 256; dst.PlacedFootprint.Footprint = { rt->fmt, 1, 1, 1, 256 };
        D3D12_TEXTURE_COPY_LOCATION src{ rt->res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; src.SubresourceIndex = 0;
        UINT x = std::min<UINT>(pts[k].first * sxx, rt->w - 1), y = std::min<UINT>(pts[k].second * msy * rt->sy, rt->h - 1);
        D3D12_BOX box{ x, y, 0, x + 1, y + 1, 1 };
        g_list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    }
    auto b = Transition(rt->res.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET); ProbedBarrier(1, &b, __LINE__);
    g_probeRecs.push_back({ g_probeDrawIdx, d.vsHash, d.psHash, Reg(0x2201), Reg(0x2001), rt->fmt, g_probeGsKind });
}
}
// GoWJ sesion 11 (tirones): el VS 45DE25B687582BDC es el "memmove" con el que UE3 desfragmenta texturas: por cada punto lee 64 bytes
// (8 x 16_16_16_16 entero) del bufer de vertices y los escribe con memexport en eA = C6 + (v - C7.x) * C255.w * C255.xyxx. En vez de
// dibujarlo en la GPU, esperar y leer los registros, se hace la misma cuenta en la CPU al grabar el draw (sin esperas).
// GOWJ_NOMEMEXCPU=1 = por la GPU (antes); GOWJ_MEMEXVERIFY=1 = las dos y se comparan (registro [memexcpu]).
static bool MemexCopyCpu(const XenosDraw& d, const ShaderEntry& vs, bool verify)
{
    if (d.primType != 1 || d.source != 2 || vs.info.vertexFetchSlots.empty()) return false;
    auto F = [](uint32_t r) { return RegF(r); };
    const float c6[4] = { F(0x4000 + 24), F(0x4001 + 24), F(0x4002 + 24), F(0x4003 + 24) };
    const float c7x = F(0x4000 + 28);
    const float c255[4] = { F(0x4000 + 1020), F(0x4001 + 1020), F(0x4002 + 1020), F(0x4003 + 1020) };
    uint32_t vf = vs.info.vertexFetchSlots[0], dw0 = Reg(0x4800 + vf * 2), dw1 = Reg(0x4800 + vf * 2 + 1);
    uint32_t src = dw0 & 0x1FFFFFFC, srcE = dw1 & 3, srcBytes = ((dw1 >> 2) & 0xFFFFFF) * 4;
    const float off = (float)Reg(0x2102);
    if (!g_pendingResolvesEmpty()) FlushResolvesIfOverlap(src, src + srcBytes);
    uint32_t written = 0, bad = 0;
    for (uint32_t i = 0; i < d.numIndices; i++)
    {
        float r0 = (float)i + off;
        uint32_t vfi = (uint32_t)std::floor(r0);
        float sc = c255[3] * (r0 - c7x);
        float ea[4] = { sc * c255[0] + c6[0], sc * c255[1] + c6[1], sc * c255[0] + c6[2], sc * c255[0] + c6[3] };
        uint32_t e[4]; memcpy(e, ea, 16);
        if ((e[1] >> 23) != 0x96 || (e[2] >> 20) != 0x4B0 || ((e[2] >> 8) & 0x3F) != 26 || ((e[2] >> 16) & 7) != 2) { bad++; continue; }
        uint32_t baseDw = e[0] & 0x3FFFFFFF, idx0 = e[1] & 0x7FFFFF, dstE = e[2] & 7, count = e[3] & 0x7FFFFF;
        for (uint32_t k = 0; k < 8; k++)
        {
            uint32_t so = vfi * 64 + k * 8; if (so + 8 > srcBytes) so = srcBytes >= 8 ? srcBytes - 8 : 0;   // como el min() del shader
            uint32_t raw0, raw1; memcpy(&raw0, g_base + 0xA0000000u + src + so, 4); memcpy(&raw1, g_base + 0xA0000000u + src + so + 4, 4);
            uint32_t p0 = Swap32(raw0, srcE), p1 = Swap32(raw1, srcE);
            // r = (u.y, u.x, u.w, u.z) y se escribe 16_16_16_16: w0 = r.x | r.y << 16, w1 = r.z | r.w << 16
            uint32_t w0 = (p0 >> 16) | ((p0 & 0xFFFF) << 16), w1 = (p1 >> 16) | ((p1 & 0xFFFF) << 16);
            uint32_t index = idx0 + k;
            if (count && index >= count) continue;
            uint64_t addr = uint64_t(baseDw) * 4 + uint64_t(index) * 8;
            if (addr + 8 > 0x20000000) continue;
            uint32_t o0 = Swap32(w0, dstE), o1 = Swap32(w1, dstE);
            if (verify) g_memexVerify.push_back({ (uint32_t)addr, uint64_t(o0) | (uint64_t(o1) << 32) });
            else { uint8_t* out = g_base + 0xA0000000u + (uint32_t)addr; memcpy(out, &o0, 4); memcpy(out + 4, &o1, 4); }
            written++;
        }
    }
    if (bad && !verify) { static int nb = 0; if (nb++ < 8) LOG("[memexcpu] %u puntos con eA inesperado: se usa la GPU", bad); return false; }
    if (!verify && written) g_cpuResolveCounter++;   // memoria cambiada: invalida las huellas de textura memorizadas
    { static uint64_t nd = 0; if ((++nd & (nd - 1)) == 0) LOG("[memexcpu] %llu copias de texturas de UE3 hechas en la CPU (ultima %u elementos)", (unsigned long long)nd, written); }
    return true;
}
static void D3D12DrawImpl(const XenosDraw& d)
{
    if (g_flushAfterResolve) { g_flushAfterResolve = false; if (g_frameOpen) { g_pendingOp = "(inicio de dibujo)"; FlushAndWait(); g_pendingOp.clear(); } }   // punto seguro: aun no se ha grabado nada del dibujo
    if (g_failed || !g_dev) return;
    OpenFrame();
    long long logFrame = DrawLogFrame();
    if ((long long)g_frameCounter == logFrame)
    {
        uint32_t tf0[6]; for (int i = 0; i < 6; i++) tf0[i] = Reg(0x4800 + i);
        LOG("[drawst] WSC=%08X/%08X SSC=%08X/%08X CC=%08X AREF=%g WOFF=%08X VPX=%g/%g VPY=%g/%g DC=%08X CMASK=%X VTE=%08X SC=%08X ZSCALE=%g ZOFF=%g tf1=%u@%08X tf2=%u@%08X tf3=%u@%08X", Reg(0x2081), Reg(0x2082), Reg(0x200E), Reg(0x200F), Reg(0x2202), RegF(0x210E), Reg(0x2080), RegF(0x210F), RegF(0x2110), RegF(0x2111), RegF(0x2112), Reg(0x2200), Reg(0x2104) & 0xF, Reg(0x2206), Reg(0x2205), RegF(0x2113), RegF(0x2114),
            Reg(0x4800 + 6 + 1) & 0x3F, Reg(0x4800 + 6 + 1) & 0xFFFFF000, Reg(0x4800 + 12 + 1) & 0x3F, Reg(0x4800 + 12 + 1) & 0xFFFFF000, Reg(0x4800 + 18 + 1) & 0x3F, Reg(0x4800 + 18 + 1) & 0xFFFFF000);
        LOG("[draw] f%llu prim=%u n=%u src=%u VS=%016llX PS=%016llX SURFACE=%08X COLOR0=%08X DEPTH=%08X MODE=%08X COPY_CTRL=%08X COPY_DEST=%08X/%08X/%08X BLEND=%08X tf0fmt=%u tf0base=%08X",
            (unsigned long long)g_frameCounter, d.primType, d.numIndices, d.source, (unsigned long long)d.vsHash, (unsigned long long)d.psHash,
            Reg(0x2000), Reg(0x2001), Reg(0x2002), Reg(0x2208), Reg(0x2318), Reg(0x2319), Reg(0x231A), Reg(0x231B), Reg(0x2201), tf0[1] & 0x3F, tf0[1] & 0xFFFFF000);
    }
    // GOWJ_DUMPDRAWS=a,b,c (con GOWJ_DRAWLOG=<frame>): vuelca las superficies EDRAM antes del draw numero a, b, c de ese frame
    {
        static std::vector<int> dumpDraws = [] {
            std::vector<int> v; const char* s = getenv("GOWJ_DUMPDRAWS");
            while (s && *s) { v.push_back(atoi(s)); s = strchr(s, ','); if (s) s++; }
            return v;
        }();
        static int drawIndex = 0;
        // GOWJ_DUMPTEXDRAW=<hex>: vuelca antes del primer draw que lee esa textura en tf0 y antes del siguiente (sesion 24)
        static uint32_t dumpTex = getenv("GOWJ_DUMPTEXDRAW") ? (uint32_t)strtoul(getenv("GOWJ_DUMPTEXDRAW"), nullptr, 16) : 0;
        static int dumpNext = 0, texHit = 0;
        // GOWJ_DUMPREL=a,b: ademas, vuelca a, b draws despues del de GOWJ_DUMPTEXDRAW
        static std::vector<int> dumpRel = [] {
            std::vector<int> v; const char* s = getenv("GOWJ_DUMPREL");
            while (s && *s) { v.push_back(atoi(s)); s = strchr(s, ','); if (s) s++; }
            return v;
        }();
        if ((long long)g_frameCounter == logFrame)
        {
            drawIndex++;
            bool hit = std::find(dumpDraws.begin(), dumpDraws.end(), drawIndex) != dumpDraws.end();
            if (texHit && std::find(dumpRel.begin(), dumpRel.end(), drawIndex - texHit) != dumpRel.end()) hit = true;
            if (dumpNext == 1) { hit = true; dumpNext = 2; }
            static uint64_t dumpPs = getenv("GOWJ_DUMPPS") ? strtoull(getenv("GOWJ_DUMPPS"), nullptr, 16) : 0;   // filtro opcional por PS
            if (dumpTex && !dumpNext && (Reg(0x4800 + 1) & 0xFFFFF000) == dumpTex && (!dumpPs || d.psHash == dumpPs) && (Reg(0x2208) & 7) == 4) { hit = true; dumpNext = 1; texHit = drawIndex; }
            // GOWJ_DUMPVS=<hash>: igual, disparado por el primer draw con ese VS (GOWJ_DUMPREL = draws siguientes a volcar) (sesion 24)
            static uint64_t dumpVs = getenv("GOWJ_DUMPVS") ? strtoull(getenv("GOWJ_DUMPVS"), nullptr, 16) : 0;
            if (dumpVs && !texHit && d.vsHash == dumpVs) { hit = true; dumpNext = 2; texHit = drawIndex; }
            if (hit) { FlushAndWait(); DumpEdram(100000 + drawIndex); }
        }
    }
    // RB_MODECONTROL (0x2208) & 7 == 6: "copy" = resolve de EDRAM a memoria (pendiente de emular), no es un draw visible
    if ((Reg(0x2208) & 7) == 6) { Resolve(); return; }
    // Topologia
    D3D12_PRIMITIVE_TOPOLOGY topo; D3D12_PRIMITIVE_TOPOLOGY_TYPE ttype; bool rect = false, expand = false;
    switch (d.primType)
    {
    case 1: { static const bool noPoints = getenv("GOWJ_NOPOINTS") != nullptr; if (noPoints) goto skipPrim; } topo = D3D_PRIMITIVE_TOPOLOGY_POINTLIST; ttype = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT; break;   // GoWJ sesion 4: puntos (memexport)
    case 2: topo = D3D_PRIMITIVE_TOPOLOGY_LINELIST; ttype = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE; break;
    case 3: topo = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP; ttype = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE; break;
    case 4: topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; ttype = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; break;
    case 6: topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; ttype = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; break;
    case 8: topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; ttype = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; rect = true; break;
    case 5: case 13: topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; ttype = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; expand = true; break;
    default:
    skipPrim:
    {
        static int logged = 0;
        if (logged < 8) { logged++; LOG("[d3d12] draw omitido: prim %u, %u indices, origen %u, VS %016llX, PS %016llX, PA_SU_POINT_SIZE=%08X MINMAX=%08X",
            d.primType, d.numIndices, d.source, (unsigned long long)d.vsHash, (unsigned long long)d.psHash, Reg(0x2280), Reg(0x2281)); }
        g_skipped[0]++; return; // puntos y otros: pendiente
    }
    }
    if (rect && !g_rectGs) return;
    // Diagnostico (sesion 24): GOWJ_SKIPFSQUAD=<tex hex> omite los quads de la interfaz a pantalla completa que leen esa textura en tf0
    if (static const char* sq = getenv("GOWJ_SKIPFSQUAD"); sq && d.vsHash == 0x56A07B413E52351Aull && (Reg(0x4800 + 1) & 0xFFFFF000) == (uint32_t)strtoul(sq, nullptr, 16))
    {
        uint32_t a = 0xA0000000u | (Reg(0x4800 + 95 * 2) & 0x1FFFFFFC);
        float x2, y2; uint32_t ux = __builtin_bswap32(*(uint32_t*)(g_base + a + 2 * 28)), uy = __builtin_bswap32(*(uint32_t*)(g_base + a + 2 * 28 + 4));
        memcpy(&x2, &ux, 4); memcpy(&y2, &uy, 4);
        if (x2 >= 1279 && y2 >= 719) return;
    }
    {   // GOWJ_SKIPSH=<hash>,<hash>... (diagnostico): omite los dibujos que usan esos VS/PS
        static std::vector<uint64_t> skipSh = [] { std::vector<uint64_t> v; if (const char* e = getenv("GOWJ_SKIPSH")) { char* p = (char*)e; while (*p) { v.push_back(strtoull(p, &p, 16)); while (*p == ',') p++; } } return v; }();
        for (uint64_t h : skipSh) if (h == d.vsHash || h == d.psHash) return;
        // GOWJ_SKIPN=<n>,<n>... (diagnostico): omite los dibujos de esos numeros de indices
        static std::vector<uint32_t> skipN = [] { std::vector<uint32_t> v; if (const char* e = getenv("GOWJ_SKIPN")) { char* p = (char*)e; while (*p) { v.push_back(strtoul(p, &p, 10)); while (*p == ',') p++; } } return v; }();
        for (uint32_t n : skipN) if (n == d.numIndices) return;
    }
    {   // GoWJ sesion 4 (diagnostico): GOWJ_MEMEXLOG=<hash VS>: constantes del draw (memexport)
        static const uint64_t mh = getenv("GOWJ_MEMEXLOG") ? strtoull(getenv("GOWJ_MEMEXLOG"), nullptr, 16) : 0; static int nl = 0;
        static uint32_t wlo = 0, whi = 0; static bool wi = [] { if (const char* s = getenv("GOWJ_TDWATCH")) { char* e; wlo = strtoul(s, &e, 16) & 0x1FFFFFFF; whi = wlo + (*e == ',' ? strtoul(e + 1, nullptr, 16) : 0x1000); } return true; }(); (void)wi;
        const uint32_t dst6 = (Reg(0x4000 + 6 * 4) & 0x3FFFFFFF) * 4;
        if (mh && d.vsHash == mh && (!whi || (dst6 >= wlo && dst6 < whi)) && nl++ < 400)
        {
            auto H = [](int c) { char b[80]; snprintf(b, sizeof(b), "c%d=%08X %08X %08X %08X ", c, Reg(0x4000 + c * 4), Reg(0x4000 + c * 4 + 1), Reg(0x4000 + c * 4 + 2), Reg(0x4000 + c * 4 + 3)); return std::string(b); };
            g_dbgMemexPending = 1; memcpy(g_dbgSkipSnap, g_skipped, sizeof(g_dbgSkipSnap));
            LOG("[memex] prim %u n %u src %u idx %08X | %s%s%s| vf95 %08X %08X | vf0 %08X %08X | VGT 2100-2103 %08X %08X %08X %08X 2180 %08X frame %llu", d.primType, d.numIndices, d.source, d.indexAddr,
                H(6).c_str(), H(7).c_str(), H(255).c_str(), Reg(0x4800 + 95 * 2), Reg(0x4800 + 95 * 2 + 1), Reg(0x4800), Reg(0x4801), Reg(0x2100), Reg(0x2101), Reg(0x2102), Reg(0x2103), Reg(0x2180), (unsigned long long)g_frameCounter);
        }
    }
    ShaderEntry& vs = GetShader(0, d.vsCode, d.vsCount, d.vsHash);
    ShaderEntry* psp = &GetShader(1, d.psCode, d.psCount, d.psHash);
    if (vs.ok && vs.info.memexport && !psp->ok && !getenv("GOWJ_ASYNCMEMEX"))
    {   // GoWJ sesion 11: draw con memexport y PS aun compilandose: se espera (la copia de memoria no se puede perder)
        for (int i = 0; i < 5000 && !psp->ok; i++) { Sleep(1); psp = &GetShader(1, d.psCode, d.psCount, d.psHash); }
        static int nw = 0; if (nw++ < 8) LOG("[memex] draw con memexport: se espero al PS %016llX (%s)", (unsigned long long)d.psHash, psp->ok ? "listo" : "NO listo");
    }
    ShaderEntry& ps = *psp;
    // Los draws que no leen memoria (sin vertices, indices ni texturas: el draw de 512 puntos que UE3 intercala tras cada copia) no
    // obligan a aplicar el memexport acumulado.
    const bool drawReadsMem = !(vs.ok && psp->ok) || !vs.info.vertexFetchSlots.empty() || d.source != 2 ||
        ((vs.info.textureMask | vs.info.texture3DMask | vs.info.textureCubeMask | psp->info.textureMask | psp->info.texture3DMask | psp->info.textureCubeMask) != 0);
    // Lote lleno: se aplica antes de grabar nada de este draw (un vaciado a mitad de draw pierde el estado de la lista)
    if (g_memexPending && vs.ok && vs.info.memexport && g_memexPendBound + d.numIndices * 16u > kMemexRecs) MemexFlushPending();
    if (vs.ok && d.vsHash == 0x45DE25B687582BDCull)
    {
        static const bool noCpu = getenv("GOWJ_NOMEMEXCPU") != nullptr, verify = getenv("GOWJ_MEMEXVERIFY") != nullptr;
        if (!noCpu)
        {
            MemexFlushPending();   // el orden con otras escrituras de memexport pendientes se mantiene
            if (!verify && MemexCopyCpu(d, vs, false)) return;
            if (verify) { g_memexVerify.clear(); MemexCopyCpu(d, vs, true); }
        }
    }
    if (g_memexPending && !(vs.ok && vs.info.memexport) && drawReadsMem)
    {
        static int nl = 0; if (nl++ < 40) LOG("[memexseq] f%llu tras %u draws de memexport (VS %016llX) llega VS %016llX PS %016llX prim %u n %u surf %08X modo %u", (unsigned long long)g_frameCounter, g_memexPendDraws, (unsigned long long)g_memexPendVs, (unsigned long long)d.vsHash, (unsigned long long)d.psHash, d.primType, d.numIndices, Reg(0x2000), Reg(0x2208) & 7);
        MemexFlushPending();   // GoWJ sesion 11: memexport acumulado
    }
    if (!vs.ok || !ps.ok)
    {   // diagnostico (sesion 10): draws omitidos porque el shader aun se compila (o no se tradujo)
        g_skipped[1]++;
        static std::unordered_map<uint64_t, int> seenSk; static int nsk = 0;
        if (nsk < 200 && seenSk[d.vsHash ^ (d.psHash * 3)]++ == 0) { nsk++; LOG("[omitido] shader pendiente f%llu VS %016llX (%d) PS %016llX (%d) SURFACE %08X COLOR0 %08X n %u", (unsigned long long)g_frameCounter, (unsigned long long)d.vsHash, (int)vs.ok, (unsigned long long)d.psHash, (int)ps.ok, Reg(0x2000), Reg(0x2001), d.numIndices); }
        return;
    }
    // Texturas resueltas por GPU que este draw leera con otro formato o tamano: se vuelcan antes a la memoria del juego (sesion 23)
    // GoWJ sesion 2: texturas leidas en el VS (UE3) + las del PS (mismo espacio de constantes de fetch)
    const uint32_t texMask2D = ps.info.textureMask | vs.info.textureMask, texMask3D = ps.info.texture3DMask | vs.info.texture3DMask,
                   texMaskCube = ps.info.textureCubeMask | vs.info.textureCubeMask;
    WriteBackResolvedForDraw(texMask2D | texMask3D | texMaskCube, texMask3D | texMaskCube);   // GoWJ: 3D/cubo se leen de memoria (LUT apilada)

    // Blend (RB_BLENDCONTROL0 = 0x2201)
    uint32_t bc = Reg(0x2201);
    // RB_COLOR_MASK (0x2104), bits 0-3 = R,G,B,A del RT0 (sesion 16). Mascara 0 = pasada solo de profundidad/stencil:
    // como aun no hay depth buffer, se omite (antes pintaba la pantalla de blanco).
    uint32_t colorMask = Reg(0x2104) & 0xF;
    // RB_MODECONTROL = 5 (kDepth): solo profundidad, sin escribir color (sesion 23). Antes pintaba de negro encima de la imagen
    // (retratos del lobby y desenfoque de profundidad de las canciones).
    if ((Reg(0x2208) & 7) == 5) colorMask = 0;
    // Sesion 19: profundidad. RB_DEPTHCONTROL (0x2200): bit0 stencil, bit1 z, bit2 escritura z, bits 4-6 funcion z,
    // bits 8-19 stencil cara frontal (func, fail, zpass, zfail), bit7 + bits 20-31 cara trasera.
    static bool noDepth = getenv("GOWJ_NODEPTH") != nullptr;
    uint32_t dc = noDepth ? 0 : Reg(0x2200);
    // PS que exporta profundidad con z desactivado (p. ej. la reduccion de profundidad de la profundidad de campo): el juego cuenta con
    // que se escriba en su superficie de profundidad, que copia justo despues. Se escribe con ALWAYS (sesion 23).
    // Sesion 24: activado por defecto (GOWJ_NOPSDEPTH=1 lo quita). Sin esto la profundidad reducida (320x180) que usan los draws de
    // velocidad (desenfoque de movimiento) quedaba vacia: no se escribian velocidades y el compositor final mezclaba los personajes
    // con una version borrosa (personajes y publico "transparentes", estelas, lavado verde en las canciones).
    static bool psDepthWrite = getenv("GOWJ_NOPSDEPTH") == nullptr;
    if (psDepthWrite && !noDepth && ps.info.writesDepth && !(dc & 2)) dc = (dc & ~0x7Eu) | 0x76u;
    {   // GOWJ_NOZFORPS=<hash PS> (diagnostico): ese PS sin prueba ni escritura de profundidad
        static const uint64_t noZPs = getenv("GOWJ_NOZFORPS") ? strtoull(getenv("GOWJ_NOZFORPS"), nullptr, 16) : 0;
        if (noZPs && d.psHash == noZPs) dc &= ~0x6u;
        static const uint32_t noZN = getenv("GOWJ_NOZFORN") ? atoi(getenv("GOWJ_NOZFORN")) : 0;   // idem para draws de n indices
        if (noZN && d.numIndices == noZN) dc &= ~0x7u;
    }
    bool useDepth = (dc & 3) != 0;
    if (!colorMask && !useDepth) { g_skipped[4]++; return; }
    // PA_SU_SC_MODE_CNTL (0x2205): bit0 cull frontal, bit1 cull trasero, bit2 cara frontal = horaria
    static bool noCull = getenv("GOWJ_NOCULL") != nullptr;
    uint32_t sc = (noCull || rect || expand) ? 0 : Reg(0x2205) & 7;
    EdramRt* curRt = GetEdramRt();
    if (!curRt) return;
    // Sesion 24: MRT. Si el PS escribe oC1 y RB_COLOR_MASK (bits 4-7) lo permite, se enlaza la superficie de RB_COLOR1_INFO como RT1
    // (el juego restaura asi el color de la escena del frame anterior a la vez que la profundidad, antes de sumar las luces).
    static bool noMrt = getenv("GOWJ_NOMRT") != nullptr;
    EdramRt* rt1 = nullptr; uint32_t mask1 = 0, bc1 = 0;
    if (!noMrt && (ps.info.mrtMask & 2) && (Reg(0x2208) & 7) != 5)
    {
        mask1 = (Reg(0x2104) >> 4) & 0xF;
        if (mask1) { rt1 = GetEdramRt(0x2003); bc1 = Reg(0x2209); }   // RB_BLENDCONTROL1
        if (rt1 == curRt || (rt1 && rt1->w != curRt->w)) rt1 = nullptr;
        static int mrtLogged = 0;
        if (mrtLogged < 8) { mrtLogged++; LOG("[d3d12] MRT: PS %016llX RT1 info %08X mascara %X blend %08X -> %s", (unsigned long long)d.psHash, Reg(0x2003), mask1, bc1, rt1 ? "enlazado" : "no"); }
    }
    // Sesion 25: tipo de GS: 1 = rect list, 2 = interfaz panoramica (superficie final de 1280 en 8888), 3 = las dos cosas
    int gsKind = rect ? 1 : 0;
    // GoWJ sesion 9: en Gears la escena es la superficie HDR (formato 3) de pitch 1280
    if ((((Reg(0x2000) & 0x3FFF) == 1120) || ((Reg(0x2000) & 0x3FFF) == 1280 && curRt->xfmt == 3)) && (dc & 2)) g_frameHasScene = true;
    {   // GoWJ sesion 11: video Bink a pantalla completa (plano Y k_8 de 1280x704). Con el panoramico el quad cubria toda la superficie
        // ancha y el video salia estirado al doble; en esos cuadros se presenta a 16:9 con bandas. GOWJ_PANOMOVIE=1 = antes.
        static const bool panoMovie = getenv("GOWJ_PANOMOVIE") != nullptr;
        if (!panoMovie && PanoWide() > 1)
            for (uint32_t t = 0; t < 3 && !g_frameHasMovie; t++)
            {
                uint32_t f0 = Reg(0x4800 + t * 6), f1 = Reg(0x4800 + t * 6 + 1), f2 = Reg(0x4800 + t * 6 + 2);
                if ((f0 & 3) == 2 && (f1 & 0x3F) == 2 && (f2 & 0x1FFF) + 1 == 1280 && ((f2 >> 13) & 0x1FFF) + 1 == 704) g_frameHasMovie = true;
                if ((f0 & 3) == 2 && (f1 & 0x3F) == 2 && (f2 & 0x1FFF) + 1 >= 640)
                {   static std::unordered_map<uint64_t, int> seenM;
                    uint64_t mk = (uint64_t(f2) << 8) | t; if (seenM.size() < 16 && seenM[mk]++ == 0)
                        LOG("[pano] textura k_8 %ux%u en tf%u (PS %016llX, superficie %08X, video %d)", (f2 & 0x1FFF) + 1, ((f2 >> 13) & 0x1FFF) + 1, t, (unsigned long long)g_curPsHash, Reg(0x2000), (int)g_frameHasMovie); }
            }
    }   // sesion 27: escena 3D = draws con prueba de profundidad en la superficie de la escena (los menus 2D no la usan)
    // Sesion 27 (2): los draws que leen una textura resuelta en este frame o el anterior (la escena) no son interfaz. El juego
    // compone la escena HDR en 1120 con una rejilla de 16 quads: el GS comprimia los del centro y dejaba los de los bordes a su
    // tamano (laterales "estirados", sin continuidad con el centro).
    bool readsResolved = false;
    if (PanoActive())
        for (uint32_t t = 0; t < 16 && !readsResolved; t++)
        {
            if (!(ps.info.textureMask & (1u << t)) || (Reg(0x4800 + t * 6) & 3) != 2) continue;
            uint32_t tb = Reg(0x4800 + t * 6 + 1) & 0xFFFFF000;
            auto lr = g_resolvedLast.find(tb);
            // Solo texturas de la escena: resueltas desde una superficie proporcional a la pantalla (de la escena). Las pequenas que el
            // juego prepara para la interfaz (iconos, barras) siguen comprimiendose (la vida de los jefes salia estirada).
            if (lr != g_resolvedLast.end()) { auto ri = g_resolved.find(lr->second); if (ri != g_resolved.end() && g_frameCounter - ri->second.frame <= 1 && ri->second.screen) readsResolved = true; }
            if (!readsResolved) { auto ld = g_resolvedDepthLast.find(tb); if (ld != g_resolvedDepthLast.end()) { auto di = g_resolvedDepth.find(ld->second); if (di != g_resolvedDepth.end() && di->second.screen) readsResolved = true; } }
        }
    if (PanoActive() && !readsResolved && g_uiGs && g_rectGsPano && ttype == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE && (((Reg(0x2000) & 0x3FFF) == 1280 && curRt->xfmt == 0) || (Reg(0x2000) & 0x3FFF) == 1120)) gsKind |= 2;   // sesion 27: el HUD se dibuja sobre la escena (1120)
    g_probeGsKind = gsKind;
    if (static const uint64_t hv = getenv("GOWJ_HUDMATLOG") ? strtoull(getenv("GOWJ_HUDMATLOG"), nullptr, 16) : 0; hv && d.vsHash == hv && (long long)g_frameCounter == DrawLogFrame())
    {   // GoWJ sesion 11 (diagnostico): matriz c6..c9 del VS de la interfaz (Scaleform) para ver si el HUD lleva perspectiva
        float m[16]; for (int i = 0; i < 16; i++) m[i] = RegF(0x4000 + 6 * 4 + i);
        LOG("[hudmat] gs %d n %u c6 %g %g %g %g | c7 %g %g %g %g | c8 %g %g %g %g | c9 %g %g %g %g", gsKind, d.numIndices, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
    }
    // GoWJ sesion 11: desplazamiento de poligonos (PA_SU_SC_MODE_CNTL bits 11-13 + PA_SU_POLY_OFFSET_*). No se aplicaba: las
    // calcomanias (sangre, impactos) se dibujan en el mismo plano que el suelo con desplazamiento y "peleaban" con el (rayas y
    // manchas que aparecian y desaparecian al moverse). DepthBias = desplazamiento x 2^24 (el registro va en 0..1), pendiente /16
    // (unidades de subpixel de Xenos, como Xenia). GOWJ_NOPOLYOFFSET=1 = antes; GOWJ_POLYOFFSCALE=<k> multiplica el desplazamiento.
    int polyBias = 0; float polySlope = 0.0f;
    {
        static const bool noPo = getenv("GOWJ_NOPOLYOFFSET") != nullptr;
        static const float poK = getenv("GOWJ_POLYOFFSCALE") ? (float)atof(getenv("GOWJ_POLYOFFSCALE")) : 1.0f;
        const uint32_t scm = Reg(0x2205);
        if (!noPo && useDepth && ((scm >> 11) & 7))
        {
            const bool front = (scm >> 11) & 1;
            const float scl = RegF(front ? 0x2380 : 0x2382), ofs = RegF(front ? 0x2381 : 0x2383);
            polyBias = (int)lrint(std::clamp(double(ofs) * 16777216.0 * poK, -1.0e8, 1.0e8)); polySlope = scl / 16.0f;   // en el registro va en unidades de profundidad (0..1): x 2^24 = unidades de D24
            static std::unordered_map<uint64_t, int> seenPo;
            uint64_t pk = (uint64_t(*(const uint32_t*)&ofs) << 32) | *(const uint32_t*)&scl;
            if (seenPo.size() < 24 && seenPo[pk]++ == 0)
                LOG("[d3d12] desplazamiento de poligonos: escala %g, desplazamiento %g -> DepthBias %d, pendiente %g (modo %08X, PS %016llX)", scl, ofs, polyBias, polySlope, scm, (unsigned long long)d.psHash);
        }
    }
    uint64_t key = (uint64_t(gsKind) * 0x2545F4914F6CDD1Dull) ^ d.vsHash * 31 ^ d.psHash ^ (uint64_t(d.primType) << 56) ^ (uint64_t(bc) << 8) ^ (uint64_t(colorMask) << 48)
                 ^ (polyBias || polySlope != 0.0f ? (uint64_t(uint32_t(polyBias)) * 0x94D049BB133111EBull) ^ (uint64_t(*(const uint32_t*)&polySlope) * 0xBF58476D1CE4E5B9ull) : 0)
                 ^ ((uint64_t(useDepth ? dc : 0) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(sc) << 60)) ^ (uint64_t(curRt->fmt) * 0xC2B2AE3D27D4EB4Full)
                 ^ (rt1 ? ((uint64_t(rt1->fmt) + 1) * 0x165667B19E3779F9ull ^ (uint64_t(mask1) << 36) ^ (uint64_t(bc1) * 0x27D4EB2F165667C5ull)) : 0);
    auto& pso = g_psos[key];
    if (!pso)
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = g_drawRootSig.Get();
        pd.VS = { vs.blob->GetBufferPointer(), vs.blob->GetBufferSize() };
        pd.PS = { ps.blob->GetBufferPointer(), ps.blob->GetBufferSize() };
        if (gsKind) { ID3DBlob* gb = gsKind == 1 ? g_rectGs.Get() : gsKind == 2 ? g_uiGs.Get() : g_rectGsPano.Get(); pd.GS = { gb->GetBufferPointer(), gb->GetBufferSize() }; }
        auto& rt = pd.BlendState.RenderTarget[0];
        uint32_t cs = bc & 0x1F, cop = (bc >> 5) & 7, cd = (bc >> 8) & 0x1F, as = (bc >> 16) & 0x1F, aop = (bc >> 21) & 7, ad = (bc >> 24) & 0x1F;
        rt.BlendEnable = !(cs == 1 && cd == 0 && cop == 0 && as == 1 && ad == 0 && aop == 0);
        rt.SrcBlend = MapBlend(cs); rt.DestBlend = MapBlend(cd); rt.BlendOp = MapOp(cop);
        rt.SrcBlendAlpha = MapBlendAlpha(as); rt.DestBlendAlpha = MapBlendAlpha(ad); rt.BlendOpAlpha = MapOp(aop);
        rt.RenderTargetWriteMask = (UINT8)colorMask;
        if (rt1)
        {
            pd.BlendState.IndependentBlendEnable = TRUE;
            auto& r1 = pd.BlendState.RenderTarget[1];
            uint32_t cs1 = bc1 & 0x1F, cop1 = (bc1 >> 5) & 7, cd1 = (bc1 >> 8) & 0x1F, as1 = (bc1 >> 16) & 0x1F, aop1 = (bc1 >> 21) & 7, ad1 = (bc1 >> 24) & 0x1F;
            r1.BlendEnable = !(cs1 == 1 && cd1 == 0 && cop1 == 0 && as1 == 1 && ad1 == 0 && aop1 == 0);
            r1.SrcBlend = MapBlend(cs1); r1.DestBlend = MapBlend(cd1); r1.BlendOp = MapOp(cop1);
            r1.SrcBlendAlpha = MapBlendAlpha(as1); r1.DestBlendAlpha = MapBlendAlpha(ad1); r1.BlendOpAlpha = MapOp(aop1);
            r1.RenderTargetWriteMask = (UINT8)mask1;
        }
        pd.SampleMask = 0xFFFFFFFF;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.DepthClipEnable = FALSE;
        pd.RasterizerState.CullMode = (sc & 3) == 1 ? D3D12_CULL_MODE_FRONT : (sc & 3) == 2 ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
        pd.RasterizerState.FrontCounterClockwise = (sc & 4) == 0;
        if (useDepth)
        {
            auto& ds = pd.DepthStencilState;
            ds.DepthEnable = (dc >> 1) & 1;
            ds.DepthWriteMask = ((dc >> 2) & 1) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
            ds.DepthFunc = MapCmp(dc >> 4);
            pd.RasterizerState.DepthBias = polyBias; pd.RasterizerState.SlopeScaledDepthBias = polySlope;   // GoWJ sesion 11
            {   // GOWJ_EQBIAS=<n> (diagnostico): segundas pasadas (z sin escritura, LEQUAL/EQUAL) adelantadas n unidades de profundidad
                static const int eqBias = getenv("GOWJ_EQBIAS") ? atoi(getenv("GOWJ_EQBIAS")) : 0;
                uint32_t zf = (dc >> 4) & 7;
                if (eqBias && ((dc >> 1) & 1) && !((dc >> 2) & 1) && (zf == 2 || zf == 3))
                {
                    static const float eqSlope = getenv("GOWJ_EQSLOPE") ? (float)atof(getenv("GOWJ_EQSLOPE")) : 0.0f;
                    pd.RasterizerState.DepthBias = -eqBias; pd.RasterizerState.SlopeScaledDepthBias = -eqSlope;
                    ds.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
                }
            }
            ds.StencilEnable = dc & 1;
            uint32_t srm = Reg(0x210C);
            ds.StencilReadMask = (UINT8)(srm >> 8); ds.StencilWriteMask = (UINT8)(srm >> 16);
            ds.FrontFace = { MapStencilOp(dc >> 11), MapStencilOp(dc >> 17), MapStencilOp(dc >> 14), MapCmp(dc >> 8) };
            ds.BackFace = ((dc >> 7) & 1) ? D3D12_DEPTH_STENCILOP_DESC{ MapStencilOp(dc >> 23), MapStencilOp(dc >> 29), MapStencilOp(dc >> 26), MapCmp(dc >> 20) } : ds.FrontFace;
            pd.DSVFormat = kDepthFmt;
        }
        pd.PrimitiveTopologyType = ttype;
        pd.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF;   // indices de reinicio = 0xFFFFFFFF
        pd.NumRenderTargets = rt1 ? 2 : 1; pd.RTVFormats[0] = curRt->fmt; pd.SampleDesc.Count = 1;
        if (rt1) pd.RTVFormats[1] = rt1->fmt;
        double tPso = QpcMs(); HRESULT hrPso = S_OK;
        static const bool syncPso = getenv("GOWJ_SYNCPSO") != nullptr;
        static const bool asyncMemexPso = getenv("GOWJ_ASYNCMEMEX") != nullptr;
        if (!TakePrewarmedPso(key, pso))
        {
            if (!syncPso && !(vs.info.memexport && !asyncMemexPso))   // GoWJ sesion 11: con memexport, PSO al momento (no omitir la copia)
            {
                if (EnqueuePso({ key, pd, d.vsHash, d.psHash, gsKind }))
                {   // se dibuja cuando este
                    g_psoAsyncSkips++; g_skipped[2]++;
                    static int npso = 0;
                    if (npso++ < 200) LOG("[omitido] PSO pendiente f%llu VS %016llX PS %016llX SURFACE %08X COLOR0 %08X n %u", (unsigned long long)g_frameCounter, (unsigned long long)d.vsHash, (unsigned long long)d.psHash, Reg(0x2000), Reg(0x2001), d.numIndices);
                    return;
                }
                hrPso = E_FAIL;
            }
            else { hrPso = CreatePsoCached(key, pd, pso); g_hPsos++; if (SUCCEEDED(hrPso)) RecordPso(key, pd, d.vsHash, d.psHash, gsKind); }
        }
        g_hPsoMs += QpcMs() - tPso;
        if (FAILED(hrPso)) { static int psoFailLog = 0; if (psoFailLog++ < 12) LOG("[d3d12] PSO fallido hr=%08X (prim %u, formato RT %d, rt1 %d) VS %016llX PS %016llX bc=%08X dc=%08X mask=%X", (unsigned)hrPso, d.primType, (int)curRt->fmt, rt1 ? (int)rt1->fmt : -1, (unsigned long long)d.vsHash, (unsigned long long)d.psHash, bc, useDepth ? dc : 0, colorMask); g_skipped[2]++; return; }
    }

    // Constantes VS (C en 0x4000) y PS (C en 0x4400) + bools + fetch + viewport
    auto buildCb = [&](uint32_t cbase, const TranslatedShader* fetchInfo, uint64_t& outOffset) -> uint8_t* {
        constexpr UINT64 kCbSize = 4864;   // sesion 24: + XLoop (32 constantes de bucle en 4320) + XTexMode[16] en 4448
        outOffset = RingAlloc(kCbSize);
        if (outOffset == ~0ull) return nullptr;
        // GoWJ sesion 10 (rendimiento): se construye en un bufer local y se copia de una vez. La memoria del anillo es de subida
        // (escritura combinada, sin cache): los "|=" y "+=" sobre ella (XTexMode.w, medio pixel, sesgo de color) eran lecturas sin
        // cache, ~130.000 por cuadro en combate (10-13 fps). GOWJ_CBDIRECT=1 = antes.
        static const bool cbDirect = getenv("GOWJ_CBDIRECT") != nullptr;
        alignas(16) static thread_local uint8_t staging[kCbSize];
        uint8_t* const ringDst = g_ringMap[g_fi] + outOffset;
        uint8_t* p = cbDirect ? ringDst : staging;
        struct CbCommit { uint8_t* dst; const uint8_t* src; bool on; ~CbCommit() { if (on) memcpy(dst, src, kCbSize); } } commit{ ringDst, staging, !cbDirect };
        uint32_t* f = (uint32_t*)p;
        uint32_t* b = (uint32_t*)(p + 4096);
        // Sesion 8 (rendimiento): copia de la sombra en orden de host (antes 1024 lecturas con cambio de endian por etapa y draw)
        extern uint32_t g_gpuConstShadow[0x1000];
        static const bool noShadow = getenv("GOWJ_NOCONSTSHADOW") != nullptr;
        if (!noShadow) { memcpy(f, &g_gpuConstShadow[cbase - 0x4000], 4096); memcpy(b, &g_gpuConstShadow[0x900], 32); }
        static const bool constCheck = getenv("GOWJ_CONSTCHECK") != nullptr;   // diagnostico: compara la sombra con los registros
        if (constCheck) { static int nbad = 0; for (uint32_t i = 0; i < 1024 && nbad < 40; i++) if (f[i] != Reg(cbase + i)) { nbad++; LOG("[constcheck] %04X sombra %08X reg %08X", cbase + i, f[i], Reg(cbase + i)); } }
        else if (noShadow) { for (uint32_t i = 0; i < 1024; i++) f[i] = Reg(cbase + i); for (uint32_t i = 0; i < 8; i++) b[i] = Reg(0x4900 + i); }   // GoWJ sesion 10: antes "else" a secas: se releian siempre los 1024 registros (14 % del hilo de la GPU)
        {   // GOWJ_PSCZERO=<hash PS>:<constante>[,<constante>...] (diagnostico, sesion 10): pone a cero esas constantes del PS
            static uint64_t zh = 0; static std::vector<int> zc = [] { std::vector<int> v; if (const char* e = getenv("GOWJ_PSCZERO")) { zh = strtoull(e, nullptr, 16); const char* c = strchr(e, ':'); while (c) { v.push_back(atoi(c + 1)); c = strchr(c + 1, ','); } } return v; }();
            if (zh && cbase == 0x4400 && g_curPsHash == zh) for (int ci : zc) if (ci >= 0 && ci < 256) memset(&f[ci * 4], 0, 16);
        }
        uint32_t* fe = (uint32_t*)(p + 4128);
        memset(fe, 0, 128);
        float* v = (float*)(p + 4256);
        uint32_t rtw = Reg(0x2000) & 0x3FFF; if (!rtw) rtw = kRtW;
        v[0] = (float)Reg(0x2206); v[1] = (float)rtw; v[2] = (float)kEdramH; v[3] = curRt->outScale;
        {   // GoWJ sesion 7: sesgo de exponente del color (RB_COLOR_INFO bits 20-25, con signo): la salida del PS se multiplica por 2^sesgo
            // al escribir en EDRAM (como Xenia). El posproceso de partida dibuja en 16_16_16_16 con +5 y resuelve con -5: sin esto
            // la exposicion, el bloom y la profundidad de campo salian 32 veces mas oscuros. GOWJ_NOCOLORBIAS=1 = antes.
            static const bool noBias = getenv("GOWJ_NOCOLORBIAS") != nullptr;
            int cb = (int)((Reg(0x2001) >> 20) & 0x3F); if (cb & 0x20) cb -= 64;
            if (!noBias && cb && cbase == 0x4400) v[3] = (v[3] != 0 ? v[3] : 1.0f) * ldexpf(1.0f, cb);
        }
        if (cbase == 0x4000)
        {   // Sesion 25: en el VS, XVte.w = escala Hor+ del panoramico en las superficies proporcionales a la pantalla (no sombras)
            static const float kx = PanoAspect() / (16.0f / 9.0f);
            // GoWJ sesion 9: la compresion la hace ya la proyeccion del juego (gancho de FPerspectiveMatrix); GOWJ_PANOVS=1 = la del VS de NG2
            static const bool panoVs = getenv("GOWJ_PANOVS") != nullptr || getenv("GOWJ_NOPANOFOV") != nullptr;
            v[3] = (panoVs && kx > 1.001f && PanoActive() && (rtw == 1120 || rtw == 560 || rtw == 280 || rtw == 1280 || rtw == 640 || rtw == 320)) ? kx : 0.0f;
        }
        {   // GOWJ_PSOUTAT=<s> (diagnostico): las sustituciones de GOWJ_PSOUT solo actuan desde ese segundo (XVte.z < 0 en el PS)
            static const double psoutAt = getenv("GOWJ_PSOUTAT") ? atof(getenv("GOWJ_PSOUTAT")) : -1;
            static const double tStart = QpcMs();
            if (psoutAt >= 0 && cbase == 0x4400 && (QpcMs() - tStart) >= psoutAt * 1000) v[2] = -1.0f;
            // GOWJ_TINTN=<n> (diagnostico, sesion 21): multiplica por 8 la salida del PS en los draws de n indices (para verlos)
            static const uint32_t tintN = getenv("GOWJ_TINTN") ? atoi(getenv("GOWJ_TINTN")) : 0;
            if (tintN && cbase == 0x4400 && d.numIndices == tintN) v[3] = (v[3] != 0 ? v[3] : 1.0f) * 8.0f;
        }
        v[4] = RegF(0x210F); v[5] = RegF(0x2110); v[6] = RegF(0x2111); v[7] = RegF(0x2112);
        v[8] = RegF(0x2113); v[9] = RegF(0x2114);
        { int wox, woy; WindowOffset(wox, woy); v[10] = (float)wox; v[11] = (float)woy; }   // PA_SC_WINDOW_OFFSET (sesion 23)
        {   // Sesion 24: PA_SU_VTX_CNTL.pix_center = 0 (convencion D3D9, centro del pixel en el entero): +0.5 en pantalla, como Xenia.
            // Sin esto cada pase a pantalla completa se corria medio pixel; en la cadena de desenfoque a 320x180 el error se
            // acumulaba y la capa borrosa quedaba desplazada de la nitida (personajes duplicados y bordes en escalera).
            static const bool noHalfPix = getenv("GOWJ_NOHALFPIXEL") != nullptr;
            static const double halfAt = getenv("GOWJ_HALFPIXAT") ? atof(getenv("GOWJ_HALFPIXAT")) : -1;   // diagnostico: activar desde el segundo N
            static const double hStart = QpcMs();
            bool on = !noHalfPix && (halfAt < 0 || QpcMs() - hStart >= halfAt * 1000);
            if (on && (Reg(0x2302) & 1) == 0) { v[10] += 0.5f; v[11] += 0.5f; }
        }
        // Prueba de alfa: RB_COLORCONTROL (0x2202) bits 0-2 funcion, bit 3 activada; RB_ALPHA_REF (0x210E) (sesion 24)
        { static bool noAt = getenv("GOWJ_NOALPHATEST") != nullptr; uint32_t cc = Reg(0x2202); v[12] = (!noAt && (cc & 8)) ? (float)(cc & 7) : 8.0f; v[13] = RegF(0x210E); v[14] = v[15] = 0; }
        {   // PsParamGen (sesion 24): SQ_PROGRAM_CNTL (0x2180) bit 18, SQ_CONTEXT_MISC (0x2181) bits 8-15 = registro destino
            static bool noPgen = getenv("GOWJ_NOPARAMGEN") != nullptr;
            uint32_t pg = (Reg(0x2181) >> 8) & 0xFF;
            v[14] = (!noPgen && ((Reg(0x2180) >> 18) & 1) && pg < 16) ? (float)pg : -1.0f;
            uint32_t pmx, pmy; MsaaScale(pmx, pmy);
            static const bool pgenHalf = getenv("GOWJ_PGENHALF") != nullptr;   // diagnostico: X - 0.5 (si el juego suma medio pixel)
            const uint32_t pws = curRt->sx, phs = curRt->sy;   // sesion 27: superficie panoramica; sy: resolucion interna
            // con superficies escaladas: bits 0-4 = muestras por pixel en X, 5-9 en Y, bit 10 = sin redondear, bit 11 = X - 0.5
            if (ScreenScaleX() > 1) v[15] = (float)((pmx * pws) | ((pmy * phs) << 5) | ((pws > 1 || phs > 1) ? 1024u + (pgenHalf ? 2048u : 0u) : 0u));
            else v[15] = (float)(pmx * pws * 8 + pmy);
        }
        { uint32_t* lp = (uint32_t*)(p + 4320); extern uint32_t g_gpuConstShadow[0x1000]; static const bool noShadow2 = getenv("GOWJ_NOCONSTSHADOW") != nullptr;
          if (!noShadow2) memcpy(lp, &g_gpuConstShadow[0x908], 128); else for (uint32_t i = 0; i < 32; i++) lp[i] = Reg(0x4908 + i); }
        {   // XTexMode[16] (sesion 24): x = clamp_x | clamp_y << 3 | clamp_z << 6, y = filtro de ampliacion, z = de reduccion, w = color de borde
            static bool noTexMode = getenv("GOWJ_NOTEXMODE") != nullptr;
            uint32_t* tm = (uint32_t*)(p + 4448);
            for (uint32_t t = 0; t < 16; t++)
            {
                uint32_t f0 = Reg(0x4800 + t * 6), f3 = Reg(0x4800 + t * 6 + 3), f5 = Reg(0x4800 + t * 6 + 5);
                tm[t * 4 + 0] = noTexMode ? (2u | (2u << 3) | (2u << 6)) : (f0 >> 10) & 0x1FF;
                tm[t * 4 + 1] = noTexMode ? 1u : (f3 >> 19) & 3;
                tm[t * 4 + 2] = noTexMode ? 1u : (((f3 >> 21) & 3) | (((f3 >> 23) & 3) << 2));
                tm[t * 4 + 3] = f5 & 3;
                {   // GoWJ sesion 10: signo por componente para el shader (XTexMode.w bits 8-15). Gamma (3) siempre en formatos de 8 bits
                    // y DXT; con signo (1) y con sesgo (2) solo con GOWJ_TEXSIGNED=1 (en prueba). GOWJ_NOTEXGAMMA=1 = sin conversion.
                    static const bool noGamma = getenv("GOWJ_NOTEXGAMMA") != nullptr, withSigned = getenv("GOWJ_TEXSIGNED") != nullptr;
                    uint32_t tfmt = Reg(0x4800 + t * 6 + 1) & 0x3F, sg = (f0 >> 2) & 0xFF, out = 0;
                    bool eightBit = tfmt == 2 || tfmt == 3 || tfmt == 4 || tfmt == 5 || tfmt == 6 || tfmt == 10 || tfmt == 14 || tfmt == 15 || (tfmt >= 18 && tfmt <= 20);
                    if (eightBit && (f0 & 3) == 2)
                        for (int k = 0; k < 4; k++) { uint32_t c = (sg >> (k * 2)) & 3; if ((c == 3 && !noGamma) || ((c == 1 || c == 2) && withSigned)) out |= c << (k * 2); }
                    tm[t * 4 + 3] |= out << 8;
                }
            }
        }   // constantes de bucle (sesion 24)
        {   // GoWJ sesion 10: XMisc (4704). x = recorte de la salida RGB en superficies 2_10_10_10_FLOAT (formato 3/12, 7e3: maximo 31,875)
            static const bool noClamp7e3 = getenv("GOWJ_NOCLAMP7E3") != nullptr;
            float* m = (float*)(p + 4704); m[0] = (!noClamp7e3 && cbase == 0x4400 && curRt->xfmt == 3) ? 31.875f : 0.0f; m[1] = m[2] = m[3] = 0;
            // GoWJ sesion 11: VGT_INDX_OFFSET en los draws sin bufer de indices (SV_VertexID empieza en 0 aunque se pase el vertice inicial)
            static const bool noAutoOff2 = getenv("GOWJ_NOAUTOOFFSET") != nullptr;
            if (cbase == 0x4000 && d.source == 2 && !expand && !noAutoOff2) m[1] = (float)Reg(0x2102);
        }
        return ringDst;   // el llamador solo escribe (el bloque de fetch de vertices); la copia se hace al salir
    };
    uint64_t vsCbOff, psCbOff;
    uint8_t* vsCb = buildCb(0x4000, &vs.info, vsCbOff);
    if (!vsCb || !buildCb(0x4400, nullptr, psCbOff)) { g_skipped[3]++; return; }
    // Streams de vertices: copiar desde la memoria fisica de invitado al buffer de subida
    uint32_t* fe = (uint32_t*)(vsCb + 4128);
    if (static const uint64_t hv = getenv("GOWJ_HUDMATLOG") ? strtoull(getenv("GOWJ_HUDMATLOG"), nullptr, 16) : 0; hv && d.vsHash == hv && (long long)g_frameCounter == DrawLogFrame() && !vs.info.vertexFetchSlots.empty())
    {   // GoWJ sesion 11 (diagnostico): posiciones (x y z w) de los primeros vertices del HUD
        uint32_t vf = vs.info.vertexFetchSlots[0], dw0 = Reg(0x4800 + vf * 2), dw1 = Reg(0x4800 + vf * 2 + 1);
        uint32_t a = 0xA0000000u | (dw0 & 0x1FFFFFFC); std::string t;
        for (uint32_t v = 0; v < 6; v++) { float f[4]; for (int i = 0; i < 4; i++) { uint32_t u = *(uint32_t*)(g_base + a + v * 48 + i * 4); if ((dw1 & 3) == 2) u = __builtin_bswap32(u); memcpy(&f[i], &u, 4); } char b[96]; snprintf(b, sizeof(b), " (%g %g %g %g)", f[0], f[1], f[2], f[3]); t += b; }
        LOG("[hudvtx] n %u endian %u:%s", d.numIndices, dw1 & 3, t.c_str());
    }
    for (size_t k = 0; k < vs.info.vertexFetchSlots.size(); k++)
    {
        uint32_t vf = vs.info.vertexFetchSlots[k];
        uint32_t dw0 = Reg(0x4800 + vf * 2), dw1 = Reg(0x4800 + vf * 2 + 1);
        uint32_t addr = dw0 & 0x1FFFFFFC, bytes = ((dw1 >> 2) & 0xFFFFFF) * 4;
        if (bytes > (8u << 20)) bytes = 8u << 20;
        // Sesion 16: el mismo vertex buffer se usa en muchos draws del frame; se copia una sola vez por frame
        // (antes se agotaban los 64 MB del anillo y se omitian miles de draws).
        uint64_t key = (uint64_t(addr) << 32) | bytes;
        UINT64 o;
        // Sesion 24: el juego reutiliza las mismas direcciones (anillo de vertices de la interfaz) con otro contenido
        // dentro del frame: la copia solo vale si la huella del contenido coincide (antes el draw recibia los vertices
        // de un quad anterior a pantalla completa y tapaba los retratos del lobby).
        if (!g_pendingResolvesEmpty()) FlushResolvesIfOverlap(addr, addr + std::max(bytes, 4u));
        const uint8_t* src = g_base + (0xA0000000u | addr);
        // GoWJ sesion 10: la huella muestreada (1 de cada nw/1024 dwords) no veia los cambios del anillo de vertices de Scaleform
        // ni de las mallas que la CPU reescribe (el arma del jugador): se reutilizaban vertices viejos -> letras hechas de trozos de
        // otras, HUD roto y el arma cian translucida. Ahora, paginas sucias (texdirty.cpp): la copia del cuadro vale mientras la CPU
        // no escriba en esas paginas. GOWJ_VBHASH=1 = huella muestreada (antes); GOWJ_VBFULLHASH=1 = huella completa (lento).
        static const bool vbDirty = TexDirtyEnabled() && getenv("GOWJ_VBHASH") == nullptr && getenv("GOWJ_VBFULLHASH") == nullptr;
        uint64_t h = 1469598103934665603ull;
        if (!vbDirty)
        {
            const uint32_t* w = (const uint32_t*)src; uint32_t nw = bytes / 4;
            static const bool vbFull = getenv("GOWJ_VBFULLHASH") != nullptr;
            uint32_t step = (vbFull || nw <= 1024) ? 1 : nw / 1024;
            for (uint32_t i = 0; i < nw; i += step) h = (h ^ w[i]) * 1099511628211ull;
            if (nw) h = (h ^ w[nw - 1]) * 1099511628211ull;
        }
        static bool noCheck = getenv("GOWJ_VBNOCHECK") != nullptr;
        auto it = g_vbCache.find(key);
        static const bool vbBoth = getenv("GOWJ_VBBOTH") != nullptr;   // diagnostico: firma de paginas Y huella completa
        uint64_t fullH = 0;
        if (vbBoth) { fullH = 1469598103934665603ull; const uint32_t* w = (const uint32_t*)src; for (uint32_t i = 0; i < bytes / 4; i++) fullH = (fullH ^ w[i]) * 1099511628211ull; }
        bool reuse = it != g_vbCache.end() && (noCheck || (vbDirty ? TexDirtyValid(addr, std::max(bytes, 4u), it->second.hash) : it->second.hash == h));
        if (vbBoth && it != g_vbCache.end() && reuse && it->second.full != fullH)
        {   static int nm = 0; if (nm++ < 30) LOG("[vbboth] %08X %u bytes: firma de paginas igual pero contenido distinto (f%llu)", addr, bytes, (unsigned long long)g_frameCounter); reuse = false; }
        if (reuse) o = it->second.off;
        else
        {
            o = RingAlloc(bytes + 16, 16);
            if (o == ~0ull) { g_skipped[3]++; return; }
            if (vbDirty) h = TexDirtyArmSig(addr, std::max(bytes, 4u));   // firma antes de copiar: una escritura posterior la cambia
            memcpy(g_ringMap[g_fi] + o, src, bytes);
            static const bool dynLog = getenv("GOWJ_DYNVBLOG") != nullptr;   // diagnostico: buferes de vertices que cambian cada frame
            if (dynLog)
            {
                static std::unordered_map<uint32_t, std::pair<uint64_t, int>> last;
                auto& e = last[addr];
                if (e.first && e.first != h && (getenv("GOWJ_DYNVBMIN") ? true : e.second++ < 2))
                {
                    const uint32_t* w = (const uint32_t*)src; uint32_t nw = bytes / 4, bad = 0;
                    for (uint32_t i = 0; i < nw; i++) { uint32_t u = __builtin_bswap32(w[i]); float f; memcpy(&f, &u, 4); if (f != f || std::isinf(f)) bad++; }
                    std::string m; for (uint32_t i = 0; i < std::min<uint32_t>(nw, 16); i++) { uint32_t u = __builtin_bswap32(w[i]); float f; memcpy(&f, &u, 4); char b[24]; snprintf(b, sizeof(b), " %.3g", f); m += b; }
                    static const double dynMin = getenv("GOWJ_DYNVBMIN") ? atof(getenv("GOWJ_DYNVBMIN")) : 0; static const DWORD dynT0 = [] { FILETIME c, e, k, u; GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u); FILETIME now; GetSystemTimeAsFileTime(&now); uint64_t a = (uint64_t(c.dwHighDateTime) << 32) | c.dwLowDateTime, b = (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime; return GetTickCount() - (DWORD)((b - a) / 10000); }();
                    if (d.vsHash == 0x9D065E47AE1B06B2ull && (GetTickCount() - dynT0) / 1000.0 >= dynMin)
                    {
                        float mn[3] = { 1e30f, 1e30f, 1e30f }, mxv[3] = { -1e30f, -1e30f, -1e30f };
                        for (uint32_t v = 0; v < nw / 8; v++) for (int k = 0; k < 3; k++) { uint32_t u = __builtin_bswap32(w[v * 8 + k]); float f; memcpy(&f, &u, 4); mn[k] = std::min(mn[k], f); mxv[k] = std::max(mxv[k], f); }
                        auto C = [](int c) { float f[4]; for (int k = 0; k < 4; k++) { uint32_t u = Reg(0x4000 + c * 4 + k); memcpy(&f[k], &u, 4); } char b[96]; snprintf(b, sizeof(b), "c%d=(%.3g %.3g %.3g %.3g) ", c, f[0], f[1], f[2], f[3]); return std::string(b); };
                        {   // centro de la tira en pantalla con c21-c24 (pos.x*c21 + pos.y*c22 + pos.z*c23 + c24)
                            float c[4][4]; for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) { uint32_t u = Reg(0x4000 + (21 + r) * 4 + k); memcpy(&c[r][k], &u, 4); }
                            float p[3] = { (mn[0] + mxv[0]) / 2, (mn[1] + mxv[1]) / 2, (mn[2] + mxv[2]) / 2 }, o[4];
                            for (int k = 0; k < 4; k++) o[k] = p[0] * c[0][k] + p[1] * c[1][k] + p[2] * c[2][k] + c[3][k];
                            LOG("[dynvb] pelo? t=%.1f centro en pantalla NDC (%.2f, %.2f) z %.3f w %.2f", (GetTickCount() - dynT0) / 1000.0, o[0] / o[3], o[1] / o[3], o[2] / o[3], o[3]);
                        }
                        LOG("[dynvb] pelo? %u vertices, x %.3g..%.3g y %.3g..%.3g z %.3g..%.3g | %s%s%s%s", nw / 8, mn[0], mxv[0], mn[1], mxv[1], mn[2], mxv[2], C(21).c_str(), C(22).c_str(), C(23).c_str(), C(24).c_str());
                    }
                    LOG("[dynvb] VS %016llX PS %016llX slot %u @%08X %u B, n %u prim %u, NaN %u |%s", (unsigned long long)d.vsHash, (unsigned long long)d.psHash, vf, addr, bytes, d.numIndices, d.primType, bad, m.c_str());
                }
                e.first = h;
            }
            static const bool nanLog = getenv("GOWJ_NANLOG") != nullptr;   // diagnostico: vertices con NaN/Inf (fisica rota en la CPU)
            if (nanLog)
            {
                const uint32_t* w = (const uint32_t*)src; uint32_t nw = bytes / 4, bad = 0;
                for (uint32_t i = 0; i < nw; i++) { uint32_t u = __builtin_bswap32(w[i]); if (((u >> 23) & 0xFF) == 0xFF && (u & 0x7FFFFF)) bad++; }
                static std::unordered_map<uint64_t, int> seen;
                if (nw && bad * 50 > nw && seen[d.vsHash]++ < 3)
                    LOG("[nan] VS %016llX PS %016llX slot %u @%08X: %u de %u palabras NaN, prim %u n %u", (unsigned long long)d.vsHash, (unsigned long long)d.psHash, vf, addr, bad, nw, d.primType, d.numIndices);
            }
            g_vbCache[key] = { o, h, fullH };
            g_vbCopyBytes += bytes;
        }
        fe[k * 4 + 0] = (uint32_t)o; fe[k * 4 + 1] = dw1 & 3; fe[k * 4 + 2] = bytes;   // z = tamano: el shader limita la lectura (sin limite, indices basura = fallo de pagina en la GPU)
    }
    // Sesion 24: en el frame de GOWJ_DRAWLOG, vertices crudos del primer stream y constantes C[111]/C[154] (shader de la interfaz)
    if ((long long)g_frameCounter == DrawLogFrame() && !vs.info.vertexFetchSlots.empty())
    {
        uint32_t vf = vs.info.vertexFetchSlots[0];
        uint32_t a = 0xA0000000u | (Reg(0x4800 + vf * 2) & 0x1FFFFFFC);
        for (uint32_t v = 0; v < std::min<uint32_t>(d.numIndices, 4); v++)
        {
            uint32_t raw[7]; for (int i = 0; i < 7; i++) raw[i] = __builtin_bswap32(*(uint32_t*)(g_base + a + v * 28 + i * 4));
            float f[7]; memcpy(f, raw, sizeof(f));
            LOG("[drawvtx] @%08X v%u pos (%g, %g, %g, %g) col %08X uv (%g, %g)", a, v, f[0], f[1], f[2], f[3], raw[4], f[5], f[6]);
        }
        auto pc = [](int n, int k) { return RegF(0x4400 + n * 4 + k); };
        {
            char buf[1200]; int k = 0;
            static const int idx[] = { 0, 1, 2, 3, 4, 5, 6, 9, 10, 20, 111, 152 };
            for (int n : idx) k += snprintf(buf + k, sizeof(buf) - k, " C%d(%g %g %g %g)", n, pc(n,0), pc(n,1), pc(n,2), pc(n,3));
            LOG("[drawps]%s", buf);
        }
        LOG("[drawvtx] C111 (%g %g %g %g) C154 (%g %g %g %g)", RegF(0x4000 + 111 * 4), RegF(0x4000 + 111 * 4 + 1), RegF(0x4000 + 111 * 4 + 2), RegF(0x4000 + 111 * 4 + 3),
            RegF(0x4000 + 154 * 4), RegF(0x4000 + 154 * 4 + 1), RegF(0x4000 + 154 * 4 + 2), RegF(0x4000 + 154 * 4 + 3));
    }
    if (d.vsHash == 0x56A07B413E52351Aull && getenv("GOWJ_UIDEBUG"))
    {
        static int n = 0;
        static DWORD lastT = 0;
        if (d.numIndices >= 40 && GetTickCount() - lastT > 3000)
        {
            lastT = GetTickCount();
            uint32_t a0 = 0xA0000000u | (Reg(0x4800 + 95 * 2) & 0x1FFFFFFC);
            uint32_t rx = __builtin_bswap32(*(uint32_t*)(g_base + a0)), ry = __builtin_bswap32(*(uint32_t*)(g_base + a0 + 4)); float fx, fy; memcpy(&fx, &rx, 4); memcpy(&fy, &ry, 4);
            LOG("[ui] t=%lu frame %llu: quads n=%u primer vertice (%g, %g)", GetTickCount(), (unsigned long long)g_frameCounter, d.numIndices, fx, fy);
        }
        static int big = 0;
        if (d.numIndices >= 40 && big++ < 0) {
            uint32_t dw0 = Reg(0x4800 + 95 * 2), dw1 = Reg(0x4800 + 95 * 2 + 1);
            uint32_t a = 0xA0000000u | (dw0 & 0x1FFFFFFC);
            for (uint32_t v = 0; v < std::min<uint32_t>(d.numIndices, 12); v++)
            {
                uint32_t raw[7]; for (int i = 0; i < 7; i++) raw[i] = __builtin_bswap32(*(uint32_t*)(g_base + a + v * 28 + i * 4));
                float px, py; memcpy(&px, &raw[0], 4); memcpy(&py, &raw[1], 4);
                LOG("[ui]   v%u pos (%g, %g) color %08X  (raw %08X %08X)", v, px, py, raw[4], *(uint32_t*)(g_base + a + v * 28), *(uint32_t*)(g_base + a + v * 28 + 16));
            }
            LOG("[ui]   PS=%016llX blend=%08X colormask=%08X tf0=%08X %08X %08X", (unsigned long long)d.psHash, Reg(0x2201), Reg(0x2104), Reg(0x4800), Reg(0x4801), Reg(0x4802));
        }
        if (n++ < 4)
        {
            uint32_t dw0 = Reg(0x4800 + 95 * 2), dw1 = Reg(0x4800 + 95 * 2 + 1);
            uint32_t a = 0xA0000000u | (dw0 & 0x1FFFFFFC);
            auto f = [&](uint32_t r) { uint32_t v = Reg(r); float x; memcpy(&x, &v, 4); return x; };
            uint32_t raw[7]; for (int i = 0; i < 7; i++) raw[i] = Load32(a + i * 4);
            float px, py, pz, pw; memcpy(&px, &raw[0], 4); memcpy(&py, &raw[1], 4); memcpy(&pz, &raw[2], 4); memcpy(&pw, &raw[3], 4);
            LOG("[ui] prim %u n %u vf95 %08X/%08X endian %u v0 = (%g %g %g %g) color %08X uv %08X %08X | VTE %08X c111 (%g %g %g %g) c154 (%g %g %g %g) vport %g %g %g %g",
                d.primType, d.numIndices, dw0, dw1, dw1 & 3, px, py, pz, pw, raw[4], raw[5], raw[6], Reg(0x2206),
                f(0x4000 + 111 * 4), f(0x4000 + 111 * 4 + 1), f(0x4000 + 111 * 4 + 2), f(0x4000 + 111 * 4 + 3),
                f(0x4000 + 154 * 4), f(0x4000 + 154 * 4 + 1), f(0x4000 + 154 * 4 + 2), f(0x4000 + 154 * 4 + 3),
                RegF(0x210F), RegF(0x2110), RegF(0x2111), RegF(0x2112));
        }
    }
    // Indices
    static thread_local std::vector<uint32_t> idxBuf;   // sesion 8 (rendimiento): sin reservar memoria en cada draw
    idxBuf.clear(); std::vector<uint32_t>& idx = idxBuf;
    bool indexed = d.source != 2 || expand;
    uint32_t indexOffset = Reg(0x2102);   // VGT_INDX_OFFSET: se suma a cada indice
    {
        static int logged = 0;
        if (indexOffset && logged < 4) { logged++; LOG("[d3d12] VGT_INDX_OFFSET = %u (draw de %u indices)", indexOffset, d.numIndices); }
    }
    if (d.source == 0)
    {
        idx.resize(d.numIndices);
        const uint8_t* src = g_base + (0xA0000000u | (d.indexAddr & 0x1FFFFFFF));
        uint32_t* id = idx.data(); const uint32_t n = d.numIndices;   // sesion 8: un bucle por caso (sin ramas por indice)
        if (!d.index32)
        {
            const uint16_t* s16 = (const uint16_t*)src;
            if (d.indexEndian == 1 || d.indexEndian == 2) for (uint32_t i = 0; i < n; i++) id[i] = uint32_t(__builtin_bswap16(s16[i])) + indexOffset;
            else for (uint32_t i = 0; i < n; i++) id[i] = uint32_t(s16[i]) + indexOffset;
        }
        else
        {
            const uint32_t* s32 = (const uint32_t*)src;
            if (d.indexEndian == 2) for (uint32_t i = 0; i < n; i++) id[i] = __builtin_bswap32(s32[i]) + indexOffset;
            else if (d.indexEndian == 3) for (uint32_t i = 0; i < n; i++) id[i] = ((s32[i] << 16) | (s32[i] >> 16)) + indexOffset;
            else for (uint32_t i = 0; i < n; i++) id[i] = s32[i] + indexOffset;   // VGT_INDX_OFFSET (sesion 21)
        }
        // Reinicio de primitiva (PA_SU_SC_MODE_CNTL bit 21 + VGT_MULTI_PRIM_IB_RESET_INDX): corta la tira. Sin esto el indice
        // 0xFFFF se tomaba como vertice 65535 y salian puas hacia lejos en el escenario (sesion 9 de NG2)
        static const bool noReset = getenv("GOWJ_NOPRIMRESET") != nullptr;
        if (!noReset && ((Reg(0x2205) >> 21) & 1))
        {
            uint32_t rst = Reg(0x2103) & (d.index32 ? 0xFFFFFFFFu : 0xFFFFu);
            for (uint32_t i = 0; i < d.numIndices; i++)
                if (idx[i] - indexOffset == rst) idx[i] = 0xFFFFFFFFu;
        }
    }
    else if (d.source == 1) idx = d.immediate;
    // GoWJ sesion 11: VGT_INDX_OFFSET tambien se suma a los indices generados (draws sin bufer de indices). UE3 mueve texturas al
    // desfragmentar con draws de puntos con memexport que avanzan solo con este desplazamiento: sin el, todas las copias repetian los
    // primeros 4 KB y la textura movida (fuente, HUD, armas, armaduras) quedaba con basura toda la partida. GOWJ_NOAUTOOFFSET=1 = antes.
    static const bool noAutoOff = getenv("GOWJ_NOAUTOOFFSET") != nullptr;
    const uint32_t autoOff = noAutoOff ? 0u : indexOffset;
    if (d.source == 2 && expand) { idx.resize(d.numIndices); for (uint32_t i = 0; i < d.numIndices; i++) idx[i] = i + autoOff; }
    else if (expand) { idx.resize(d.numIndices); for (uint32_t i = 0; i < d.numIndices; i++) idx[i] = i; }
    if (expand)
    {
        std::vector<uint32_t> out;
        if (d.primType == 5) for (size_t i = 2; i < idx.size(); i++) { out.push_back(idx[0]); out.push_back(idx[i - 1]); out.push_back(idx[i]); }
        else for (size_t i = 0; i + 3 < idx.size() + 0 && i + 3 <= idx.size() - 1 + 1; i += 4) { if (i + 3 >= idx.size()) break; out.insert(out.end(), { idx[i], idx[i + 1], idx[i + 2], idx[i], idx[i + 2], idx[i + 3] }); }
        idx.swap(out);
    }

    EdramRt* rt = GetEdramRt();
    if (!rt) return;
    D3D12_CPU_DESCRIPTOR_HANDLE edramRtv = Rtv(rt->rtv);
    // Sesion 24: aliasing de EDRAM. Un rectangulo de solo profundidad (modo 5, z siempre y con escritura) cuya base de profundidad
    // coincide con la del color escribe D24S8 encima del color en la consola: el juego lo usa para borrar la escena a negro antes
    // de pintar los brillos del bloom (y en los retratos). Aqui color y profundidad son recursos distintos: se aplica al color.
    static bool noAlias = getenv("GOWJ_NODEPTHALIAS") != nullptr;
    if (!noAlias && (Reg(0x2208) & 7) == 5 && (Reg(0x2002) & 0xFFF) == (Reg(0x2001) & 0xFFF) && getenv("GOWJ_ALIASLOG"))
    {
        static int nl = 0;
        if (nl++ < 80) LOG("[edram] alias candidato: surf %08X rect %d prim %u dc %08X stencilref %08X", Reg(0x2000), (int)rect, d.primType, dc, Reg(0x210C));
    }
    if (!noAlias && rect && (Reg(0x2208) & 7) == 5 && (dc & 6) == 6 && ((dc >> 4) & 7) == 7 &&
        (Reg(0x2002) & 0xFFF) == (Reg(0x2001) & 0xFFF) && rt->fmt == DXGI_FORMAT_B8G8R8A8_UNORM && !vs.info.vertexFetchSlots.empty() &&
        (dc & 1) && ((dc >> 8) & 7) == 7 && ((dc >> 14) & 7) == 2)   // stencil "siempre" + "reemplazar": se escriben los 32 bits
    {
        uint32_t stride = 8;
        size_t sp = vs.info.hlsl.find("; vfStride = ");   // la asignacion del fetch (no la declaracion "uint vfStride = 0")
        if (sp != std::string::npos) stride = (uint32_t)strtoul(vs.info.hlsl.c_str() + sp + 13, nullptr, 10);
        if (stride < 8) stride = 8;
        uint32_t vf = vs.info.vertexFetchSlots[0], vdw0 = Reg(0x4800 + vf * 2), vdw1 = Reg(0x4800 + vf * 2 + 1);
        const uint8_t* vb = g_base + (0xA0000000u | (vdw0 & 0x1FFFFFFC));
        float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f, z = 0;
        for (int i = 0; i < 3; i++)
        {
            float f[3];
            for (int k = 0; k < 3; k++) { uint32_t u = *(const uint32_t*)(vb + i * stride + k * 4); if ((vdw1 & 3) == 2) u = __builtin_bswap32(u); memcpy(&f[k], &u, 4); }
            minx = std::min(minx, f[0]); maxx = std::max(maxx, f[0]); miny = std::min(miny, f[1]); maxy = std::max(maxy, f[1]);
            if (i == 0) z = stride >= 12 ? f[2] : 0.0f;
        }
        z = z * RegF(0x2113) + RegF(0x2114);   // ZSCALE / ZOFF (sin efecto si el VTE no los usa: aqui solo se usan como aproximacion)
        if (!(z >= -0.01f && z <= 1.01f)) z = 0;
        uint32_t stencil = (dc & 1) ? (Reg(0x210C) & 0xFF) : 0;
        uint32_t dw = ((uint32_t)(std::clamp(z, 0.0f, 1.0f) * 16777215.0f + 0.5f) << 8) | stencil;
        // Disposicion de k_8_8_8_8 en la EDRAM: R en los bits 0-7 ... A en 24-31
        float col[4] = { (dw & 0xFF) / 255.0f, ((dw >> 8) & 0xFF) / 255.0f, ((dw >> 16) & 0xFF) / 255.0f, (dw >> 24) / 255.0f };
        uint32_t amsx, amsy; MsaaScale(amsx, amsy); amsx *= rt->sx; amsy *= rt->sy;
        int wox, woy; WindowOffset(wox, woy);
        D3D12_RECT r{ (LONG)std::max(0.0f, (minx + 0.5f + wox) * amsx), (LONG)std::max(0.0f, (miny + 0.5f + woy) * amsy),
                      (LONG)std::min((float)rt->w, (maxx + 0.5f + wox) * amsx), (LONG)std::min((float)rt->h, (maxy + 0.5f + woy) * amsy) };
        if (r.right > r.left && r.bottom > r.top) PROBED(ClearRenderTargetView(edramRtv, col, 1, &r));
        static int logged = 0;
        if (logged < 60) { logged++; LOG("[edram] borrado por aliasing profundidad->color: surf %08X f%llu base %u, rect %ld,%ld-%ld,%ld, z %g, dword %08X (stride %u, endian %u, x %g..%g, y %g..%g)", Reg(0x2000), (unsigned long long)g_frameCounter, rt->base, r.left, r.top, r.right, r.bottom, z, dw, stride, vdw1 & 3, minx, maxx, miny, maxy); }
    }
    DepthRt* drt = useDepth ? GetDepthRt() : nullptr;
    if (useDepth && !drt) return;
    D3D12_CPU_DESCRIPTOR_HANDLE dsvH = drt ? Dsv(drt->dsv) : D3D12_CPU_DESCRIPTOR_HANDLE{};
    if (rt1) { D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2] = { edramRtv, Rtv(rt1->rtv) }; PROBED(OMSetRenderTargets(2, rtvs, FALSE, drt ? &dsvH : nullptr)); }
    else PROBED(OMSetRenderTargets(1, &edramRtv, FALSE, drt ? &dsvH : nullptr));
    if (drt) PROBED(OMSetStencilRef(Reg(0x210C) & 0xFF));
    g_boundRt = rt;
    // Viewport en muestras: el VS mapea pixeles logicos (pitch x kEdramH) y el viewport los escala por el MSAA
    uint32_t msx, msy; MsaaScale(msx, msy);
    D3D12_VIEWPORT vp{ 0, 0, (float)rt->w, (float)(kEdramH * msy * rt->sy), 0, 1 }; D3D12_RECT sr{ 0, 0, (LONG)rt->w, (LONG)rt->h };
    // Sesion 24: scissor (como GetScissor de Xenia): PA_SC_WINDOW_SCISSOR (+ PA_SC_WINDOW_OFFSET salvo window_offset_disable)
    // ∩ PA_SC_SCREEN_SCISSOR. La interfaz recorta con el las "ventanas" (p. ej. el viewport de fondo del lobby, fuera de pantalla).
    static bool noScissor = getenv("GOWJ_NOSCISSOR") != nullptr;
    // GOWJ_SCISSORBIG=1: solo en superficies de >= 640 de ancho (prueba; el resultado en canciones fue el mismo)
    static bool scissorBig = getenv("GOWJ_SCISSORBIG") != nullptr;
    uint32_t sPitch = Reg(0x2000) & 0x3FFF;
    if (!noScissor && (!scissorBig || sPitch >= 640))
    {
        uint32_t wtl = Reg(0x2081), wbr = Reg(0x2082), stl = Reg(0x200E), sbr = Reg(0x200F), wo = Reg(0x2080);
        auto s15 = [](uint32_t v) { return (int32_t)(v << 17) >> 17; };
        int32_t tlx = (int32_t)(wtl & 0x3FFF), tly = (int32_t)((wtl >> 16) & 0x3FFF), brx = (int32_t)(wbr & 0x3FFF), bry = (int32_t)((wbr >> 16) & 0x3FFF);
        if (!(wtl >> 31)) { int32_t ox = s15(wo & 0x7FFF), oy = s15((wo >> 16) & 0x7FFF); tlx += ox; tly += oy; brx += ox; bry += oy; }
        tlx = std::max(tlx, s15(stl & 0x7FFF)); tly = std::max(tly, s15((stl >> 16) & 0x7FFF));
        brx = std::min(brx, s15(sbr & 0x7FFF)); bry = std::min(bry, s15((sbr >> 16) & 0x7FFF));
        tlx = std::max(tlx, 0); tly = std::max(tly, 0); brx = std::max(brx, tlx); bry = std::max(bry, tly);
        const LONG msxS = (LONG)(msx * rt->sx), msyS = (LONG)(msy * rt->sy);   // sesion 27: superficie panoramica; sy: resolucion interna
        sr.left = std::min<LONG>(tlx * msxS, (LONG)rt->w); sr.top = std::min<LONG>(tly * msyS, (LONG)rt->h);
        sr.right = std::min<LONG>(brx * msxS, (LONG)rt->w); sr.bottom = std::min<LONG>(bry * msyS, (LONG)rt->h);
        // La interfaz comprimida al centro por el GS: su recorte tambien (salvo el de pantalla completa)
        if ((gsKind & 2) && (sr.left > 0 || sr.right < (LONG)rt->w))
        {
            static const float kxs = PanoAspect() / (16.0f / 9.0f);
            const float c = rt->w * 0.5f;
            sr.left = (LONG)std::floor(c + (sr.left - c) / kxs); sr.right = (LONG)std::ceil(c + (sr.right - c) / kxs);
        }
    }
    PROBED(RSSetViewports(1, &vp)); PROBED(RSSetScissorRects(1, &sr));
    g_list->SetPipelineState(pso.Get());
    g_list->SetGraphicsRootSignature(g_drawRootSig.Get());
    ID3D12DescriptorHeap* heaps[] = { g_srvHeap.Get() };
    g_list->SetDescriptorHeaps(1, heaps);
    D3D12_GPU_VIRTUAL_ADDRESS ringVa = g_ring[g_fi]->GetGPUVirtualAddress();
    PROBED(SetGraphicsRootConstantBufferView(0, ringVa + vsCbOff));
    PROBED(SetGraphicsRootConstantBufferView(1, ringVa + psCbOff));
    PROBED(SetGraphicsRootShaderResourceView(2, ringVa));
    const bool memex = vs.info.memexport && !getenv("GOWJ_NOMEMEXPORT");
    static const bool memexSync = getenv("GOWJ_MEMEXSYNC") != nullptr || getenv("GOWJ_MEMEXVERIFY") != nullptr;
    const uint32_t memexBound = d.numIndices * 16u;   // cota de registros del draw (hasta 16 escrituras eM por vertice)
    if (memex && g_memexPending)
    {   // se acumula sobre los registros de los draws anteriores (sin poner el contador a cero)
        D3D12_RESOURCE_BARRIER ub{}; ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; ub.UAV.pResource = g_memexBuf.Get(); g_list->ResourceBarrier(1, &ub);
        MemexBind(false);
    }
    else MemexBind(memex);
    {
        // Sesion 18: antes 8192 descriptores por frame = solo 512 draws; los demas (la interfaz, que va al final) salian blancos
        UINT baseSlot = 32 + g_fi * kDescPerFrame + g_descCursor;
        if (g_descCursor + kDescPerDraw > kDescPerFrame) { baseSlot = 1; g_skipped[5]++; } // sin descriptores: todo blanco
        else
        {
            g_descCursor += kDescPerDraw;
            static bool noTex = getenv("GOWJ_NOTEXTURES") != nullptr;
            struct TexTimer { double t0 = QpcMs(); ~TexTimer() { g_hTexMs += QpcMs() - t0; } } ttimer;
            // GoWJ sesion 10 (rendimiento): las 48 ranuras se rellenan de una vez copiando un bloque fijo (16 blancas 2D, 16 nulas 3D,
            // 16 nulas cubo) y solo se crean los SRV de las ranuras que usa el shader. Antes eran 48 CreateShaderResourceView por draw
            // (~190.000 por cuadro en combate). GOWJ_ALLSRV=1 = antes.
            static const bool allSrv = getenv("GOWJ_ALLSRV") != nullptr;
            static ComPtr<ID3D12DescriptorHeap> defaults;
            if (!allSrv && !defaults)
            {
                D3D12_DESCRIPTOR_HEAP_DESC dd{}; dd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; dd.NumDescriptors = 48;
                if (SUCCEEDED(g_dev->CreateDescriptorHeap(&dd, IID_PPV_ARGS(&defaults))))
                {
                    auto at = [&](UINT i) { auto h = defaults->GetCPUDescriptorHandleForHeapStart(); h.ptr += i * g_srvSize; return h; };
                    for (UINT i = 0; i < 16; i++)
                    {
                        g_dev->CreateShaderResourceView(g_white.Get(), nullptr, at(i));
                        D3D12_SHADER_RESOURCE_VIEW_DESC nd{}; nd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
                        nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; nd.Texture3D.MipLevels = 1;
                        g_dev->CreateShaderResourceView(nullptr, &nd, at(16 + i));
                        nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE; nd.TextureCube.MipLevels = 1;
                        g_dev->CreateShaderResourceView(nullptr, &nd, at(32 + i));
                    }
                }
            }
            const bool fast = !allSrv && defaults;
            if (fast) g_dev->CopyDescriptorsSimple(48, SrvCpu(baseSlot), defaults->GetCPUDescriptorHandleForHeapStart(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            const uint32_t usedAll = texMask2D | texMask3D | texMaskCube;
            for (uint32_t t = 0; t < 16; t++)
            {
                if (fast && !(usedAll & (1u << t)) && (long long)g_frameCounter != DrawLogFrame()) continue;
                bool ok = (texMask2D & (1u << t)) && !noTex && BindTexture(t, SrvCpu(baseSlot + t));
                if (ok && (g_bindSx > 1 || g_bindSy > 1)) ((uint32_t*)(g_ringMap[g_fi] + psCbOff + 4448))[t * 4 + 2] |= ((g_bindSx * 16u) << 8) | ((g_bindSy * 16u) << 16);   // sesion 27 (+ Y: resolucion interna)
                if (!ok && !(fast && !(texMask2D & (1u << t)))) g_dev->CreateShaderResourceView(g_white.Get(), nullptr, SrvCpu(baseSlot + t));
                // Huecos 3D (t17-t32): textura de volumen o, si no hay, un SRV nulo 3D
                bool ok3 = (texMask3D & (1u << t)) && !noTex && BindTexture3D(t, SrvCpu(baseSlot + 16 + t));
                if (!ok3 && !fast)
                {
                    D3D12_SHADER_RESOURCE_VIEW_DESC nd{}; nd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
                    nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; nd.Texture3D.MipLevels = 1;
                    g_dev->CreateShaderResourceView(nullptr, &nd, SrvCpu(baseSlot + 16 + t));
                }
                // Huecos cubo (t33-t48)
                bool okc = (texMaskCube & (1u << t)) && !noTex && BindTextureCube(t, SrvCpu(baseSlot + 32 + t));
                if (!okc && !fast)
                {
                    D3D12_SHADER_RESOURCE_VIEW_DESC nd{}; nd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                    nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; nd.TextureCube.MipLevels = 1;
                    g_dev->CreateShaderResourceView(nullptr, &nd, SrvCpu(baseSlot + 32 + t));
                }
                // Registro de texturas por draw en el frame de GOWJ_DRAWLOG (sesion 23)
                uint32_t used = (texMask2D | texMask3D | texMaskCube) & (1u << t);
                if (used && (long long)g_frameCounter == DrawLogFrame())
                {
                    uint32_t c0 = Reg(0x4800 + t * 6), c1 = Reg(0x4800 + t * 6 + 1), c2 = Reg(0x4800 + t * 6 + 2), c5 = Reg(0x4800 + t * 6 + 5);
                    LOG("[drawtex] tf%u dw0 %08X dw3 %08X dw4 %08X dw5 %08X endian %u uso %s dim %u fmt %u base %08X tam %08X tipo %u -> %s", t, c0, Reg(0x4800 + t * 6 + 3), Reg(0x4800 + t * 6 + 4), c5, (c1 >> 6) & 3,
                        (texMask3D >> t) & 1 ? "3D" : (texMaskCube >> t) & 1 ? "cubo" : "2D",
                        (c5 >> 9) & 3, c1 & 0x3F, c1 & 0xFFFFF000, c2, c0 & 3, (ok || ok3 || okc) ? "cargada" : "BLANCA/NULA");
                }
            }
        }
        PROBED(SetGraphicsRootDescriptorTable(3, SrvGpu(baseSlot)));
    }
    CmdTrace("draw p%u n%u idx %zu indexado %d rect %d rt %ux%u fmt %d depth %d", d.primType, d.numIndices, idx.size(), (int)indexed, (int)rect, rt->w, rt->h, (int)rt->fmt, (int)useDepth);
    PROBED(IASetPrimitiveTopology(topo));
    if (indexed)
    {
        if (idx.empty()) return;
        UINT64 o = RingAlloc(idx.size() * 4, 16);
        if (o == ~0ull) { g_skipped[3]++; return; }
        memcpy(g_ringMap[g_fi] + o, idx.data(), idx.size() * 4);
        D3D12_INDEX_BUFFER_VIEW ib{ ringVa + o, (UINT)(idx.size() * 4), DXGI_FORMAT_R32_UINT };
        PROBED(IASetIndexBuffer(&ib));
        g_list->DrawIndexedInstanced((UINT)idx.size(), 1, 0, 0, 0);
    }
    else g_list->DrawInstanced(d.numIndices, 1, 0, 0);   // GoWJ sesion 11: el desplazamiento va en XMisc.y (SV_VertexID no incluye el vertice inicial)
    g_frameDraws++;
    ProbeAfterDraw(rt, d);   // diagnostico GOWJ_PIXELPROBE (sesion 10)
    if (memex)
    {
        if (memexSync) MemexProcess(d.vsHash, memexBound);
        else { g_memexPending = true; g_memexPendBound += memexBound; g_memexPendDraws++; g_memexPendVs = d.vsHash; }
    }
    {   // Aliasing de EDRAM color -> profundidad (sesion 24): color 8888 escrito en la base del buffer de profundidad (modo 4)
        static const bool noC2D0 = getenv("GOWJ_NOCOLOR2DEPTH") != nullptr;
        static const double c2dOffAt = getenv("GOWJ_C2DOFFAT") ? atof(getenv("GOWJ_C2DOFFAT")) : -1;   // diagnostico: quitarlo desde el segundo N
        static const double c2dStart = QpcMs();
        const bool noC2D = noC2D0 || (c2dOffAt >= 0 && QpcMs() - c2dStart >= c2dOffAt * 1000);
        if (!noC2D && g_colorToDepthPso && rt->fmt == DXGI_FORMAT_B8G8R8A8_UNORM && (Reg(0x2208) & 7) == 4 &&
            (Reg(0x2001) & 0xFFF) == (Reg(0x2002) & 0xFFF) && (Reg(0x2104) & 0xF) != 0)
        {
            if (DepthRt* zr = GetDepthRt())
            {
                if (zr->w == rt->w)
                {
                    D3D12_RESOURCE_BARRIER b0 = Transition(rt->res.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    ProbedBarrier(1, &b0, __LINE__);
                    UINT zslot = 6 + g_fi;
                    if (g_descCursor + 1 <= kDescPerFrame) { zslot = 32 + g_fi * kDescPerFrame + g_descCursor; g_descCursor += 1; }
                    D3D12_SHADER_RESOURCE_VIEW_DESC zsd{}; zsd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; zsd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    zsd.Texture2D.MipLevels = 1; zsd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    g_dev->CreateShaderResourceView(rt->res.Get(), &zsd, SrvCpu(zslot));
                    D3D12_CPU_DESCRIPTOR_HANDLE zdsv = Dsv(zr->dsv);
                    PROBED(OMSetRenderTargets(0, nullptr, FALSE, &zdsv));
                    g_boundRt = nullptr;
                    D3D12_VIEWPORT zvp{ 0, 0, (float)rt->w, (float)rt->h, 0, 1 };
                    PROBED(RSSetViewports(1, &zvp)); PROBED(RSSetScissorRects(1, &sr));
                    g_list->SetPipelineState(rt->sx > 1 && g_colorToDepthPsoK ? g_colorToDepthPsoK.Get() : g_colorToDepthPso.Get());
                    g_list->SetGraphicsRootSignature(g_colorToDepthRootSig.Get());
                    ID3D12DescriptorHeap* zheaps[] = { g_srvHeap.Get() };
                    g_list->SetDescriptorHeaps(1, zheaps);
                    PROBED(SetGraphicsRootDescriptorTable(0, SrvGpu(zslot)));
                    PROBED(IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST));
                    g_list->DrawInstanced(3, 1, 0, 0);
                    D3D12_RESOURCE_BARRIER b1 = Transition(rt->res.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                    ProbedBarrier(1, &b1, __LINE__);
                    static int logged = 0;
                    if (logged++ < 4) LOG("[edram] aliasing color->profundidad: base %u, pitch %u, rect %ld,%ld-%ld,%ld (PS %016llX)", rt->base, rt->w, sr.left, sr.top, sr.right, sr.bottom, (unsigned long long)d.psHash);
                }
            }
        }
    }
}

// GOWJ_DUMPFRAME=n: guarda el render target EDRAM del frame n en frame_n.bmp
static void DumpEdram(UINT64 frame)
{
    // Vuelca TODAS las superficies EDRAM (recortadas a 720 de alto): edram_<frame>_b<base>_p<pitch>.bmp
    for (auto& kv : g_edramRts)
    {
        EdramRt& rt = kv.second;
        // 8888 tal cual; R32_FLOAT y R16G16_SNORM se convierten a gris/rojo-verde para diagnostico (sesion 23)
        bool isR32 = rt.fmt == DXGI_FORMAT_R32_FLOAT, isRG16 = rt.fmt == DXGI_FORMAT_R16G16_SNORM;
        bool isRGBA16 = rt.fmt == DXGI_FORMAT_R16G16B16A16_SNORM || rt.fmt == DXGI_FORMAT_R16G16B16A16_FLOAT;   // GoWJ: HDR (x32 / half)
        if (rt.fmt != DXGI_FORMAT_B8G8R8A8_UNORM && !isR32 && !isRG16 && !isRGBA16) continue;
        static int onlyPitch = getenv("GOWJ_DUMPPITCH") ? atoi(getenv("GOWJ_DUMPPITCH")) : 0;   // filtro opcional por pitch
        if (onlyPitch && (int)rt.w != onlyPitch) continue;
        UINT w = rt.w, h = std::min<UINT>(rt.h, kRtH), rowPitch = (w * (isRGBA16 ? 8 : 4) + 255) & ~255u;
        D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = UINT64(rowPitch) * h; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES rb{ D3D12_HEAP_TYPE_READBACK };
        ComPtr<ID3D12Resource> buf;
        if (FAILED(g_dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buf)))) continue;
        ComPtr<ID3D12CommandAllocator> a; ComPtr<ID3D12GraphicsCommandList> l;
        g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
        g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a.Get(), nullptr, IID_PPV_ARGS(&l));
        auto t1 = Transition(rt.res.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE); l->ResourceBarrier(1, &t1);
        D3D12_TEXTURE_COPY_LOCATION dst{ buf.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; dst.PlacedFootprint.Footprint = { rt.fmt, w, h, 1, rowPitch };
        D3D12_TEXTURE_COPY_LOCATION src{ rt.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; src.SubresourceIndex = 0;
        D3D12_BOX dbox{ 0, 0, 0, w, h, 1 };
        l->CopyTextureRegion(&dst, 0, 0, 0, &src, &dbox);
        auto t2 = Transition(rt.res.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET); l->ResourceBarrier(1, &t2);
        l->Close();
        ID3D12CommandList* ls[] = { l.Get() }; g_queue->ExecuteCommandLists(1, ls);
        g_queue->Signal(g_fence.Get(), g_nextFence); g_fence->SetEventOnCompletion(g_nextFence++, g_fenceEvent); WaitForSingleObject(g_fenceEvent, 5000);
        uint8_t* p = nullptr; buf->Map(0, nullptr, (void**)&p);
        std::vector<uint8_t> conv;
        if (isRGBA16)
        {
            bool sn = rt.fmt == DXGI_FORMAT_R16G16B16A16_SNORM; float mx = 0;
            auto get = [&](const uint8_t* q, int k) { uint16_t r; memcpy(&r, q + k * 2, 2); if (sn) return std::max(-1.0f, (int16_t)r / 32767.0f) * 32.0f;
                uint32_t e = (r >> 10) & 31, m = r & 1023; float v = e ? ldexpf(1.0f + m / 1024.0f, (int)e - 15) : ldexpf(m / 1024.0f, -14); return (r & 0x8000) ? -v : v; };
            UINT rp2 = (w * 4 + 255) & ~255u; conv.resize(size_t(rp2) * h);
            for (UINT y = 0; y < h; y++) for (UINT x = 0; x < w; x++) { const uint8_t* q = p + y * rowPitch + x * 8; for (int k = 0; k < 3; k++) mx = std::max(mx, get(q, k)); }
            LOG("[edram] volcado RGBA16%s b%u p%u: max %g", sn ? "(x32)" : "F", rt.base, rt.w, mx);
            for (UINT y = 0; y < h; y++) for (UINT x = 0; x < w; x++)
            {
                const uint8_t* q = p + y * rowPitch + x * 8; uint8_t* o = conv.data() + y * rp2 + x * 4;
                for (int k = 0; k < 3; k++) { float v = get(q, k); v = v / (1.0f + v); o[2 - k] = (uint8_t)std::clamp(powf(std::max(v, 0.0f), 1 / 2.2f) * 255.0f, 0.0f, 255.0f); }
                o[3] = 255;
            }
            buf->Unmap(0, nullptr); p = conv.data(); rowPitch = rp2;
        }
        if (isR32 || isRG16)
        {
            float mn = 1e30f, mx = -1e30f;
            for (UINT y = 0; y < h; y++) for (UINT x = 0; x < w; x++) { float v; if (isR32) memcpy(&v, p + y * rowPitch + x * 4, 4); else v = *(int16_t*)(p + y * rowPitch + x * 4) / 32767.0f; mn = std::min(mn, v); mx = std::max(mx, v); }
            LOG("[edram] volcado %s b%u p%u: min %g max %g", isR32 ? "R32F" : "RG16", rt.base, rt.w, mn, mx);
            for (UINT y = 0; y < h; y++) for (UINT x = 0; x < w; x++)
            {
                uint8_t* q = p + y * rowPitch + x * 4; uint8_t R, G;
                if (isR32) { float v; memcpy(&v, q, 4); R = G = (uint8_t)std::clamp((v - mn) / std::max(mx - mn, 1e-20f) * 255.0f, 0.0f, 255.0f); }
                else { R = (uint8_t)std::clamp(*(int16_t*)q / 32767.0f * 127.5f + 127.5f, 0.0f, 255.0f); G = (uint8_t)std::clamp(*(int16_t*)(q + 2) / 32767.0f * 127.5f + 127.5f, 0.0f, 255.0f); }
                q[0] = isR32 ? R : 0; q[1] = G; q[2] = R; q[3] = 255;
            }
        }
        char name[96]; snprintf(name, sizeof(name), "edram_%llu_b%u_p%u%s.bmp", (unsigned long long)frame, rt.base, rt.w, isR32 ? "_r32f" : isRG16 ? "_rg16" : isRGBA16 ? "_hdr" : "");
        if (FILE* f = fopen(name, "wb"))
        {
            uint32_t size = w * h * 4;
            uint8_t hdr[54] = { 'B', 'M' }; *(uint32_t*)(hdr + 2) = 54 + size; *(uint32_t*)(hdr + 10) = 54; *(uint32_t*)(hdr + 14) = 40;
            *(int32_t*)(hdr + 18) = (int32_t)w; *(int32_t*)(hdr + 22) = -(int32_t)h; *(uint16_t*)(hdr + 26) = 1; *(uint16_t*)(hdr + 28) = 32; *(uint32_t*)(hdr + 34) = size;
            fwrite(hdr, 1, 54, f);
            for (UINT y = 0; y < h; y++) fwrite(p + y * rowPitch, 1, w * 4, f);
            fclose(f);
        }
        if (!isRGBA16) buf->Unmap(0, nullptr);
    }
    LOG("[d3d12] volcadas %zu superficies EDRAM del frame %llu", g_edramRts.size(), (unsigned long long)frame);
}

// Presenta un frame: si hubo draws, el render target EDRAM; si no, el frontbuffer (BGRA w x h) o nada.
// Presenta directamente la textura resuelta por GPU del frontbuffer (sesion 21). Devuelve false si no hay (se usa la memoria).
// readback != nullptr: ademas copia la imagen a la CPU (BGRA, como ReadFrontbuffer) para las capturas.
bool D3D12Present(HWND hwnd, const uint32_t* pixels, uint32_t w, uint32_t h);
bool D3D12FlushPendingResolves() { if (!g_dev || g_pendingResolvesEmpty()) return false; FlushPendingResolves(); return true; }

bool D3D12PresentResolved(HWND hwnd, uint32_t fb, std::vector<uint32_t>* readback, uint32_t& outW, uint32_t& outH)
{
    if (g_dev) MemexFlushPending();   // GoWJ sesion 11
    if (g_dev) FlushPendingResolves();
    {   // GoWJ sesion 2: GOWJ_DUMPEDRAMAT=<segundos>: vuelca las superficies EDRAM una vez (tambien en presentacion directa)
        static const char* da = getenv("GOWJ_DUMPEDRAMAT"); static DWORD t0 = GetTickCount(); static bool done = false;
        if (da && !done && g_dev && GetTickCount() - t0 >= (DWORD)(atof(da) * 1000)) { done = true; FlushAndWait(); DumpEdram(999999); }
    }
    if (g_failed || !g_dev || !hwnd || !fb) return false;
    auto last = g_resolvedLast.find(fb & 0xFFFFF000);
    if (last == g_resolvedLast.end()) return false;
    auto it = g_resolved.find(last->second);
    if (it == g_resolved.end() || !it->second.res || g_frameCounter - it->second.frame > 2) return false;
    ResolvedTex& r = it->second;
    OpenFrame();
    UINT fi = g_fi;
    const UINT rw = r.w * (r.sx ? r.sx : 1);   // sesion 27: frontbuffer panoramico (mas ancho)
    const UINT rh = r.h * (r.sy ? r.sy : 1);   // GoWJ sesion 11: y mas alto (resolucion interna)
    outW = rw; outH = rh;
    if (readback)
    {
        // Copia a la CPU para la captura
        if (r.state != D3D12_RESOURCE_STATE_COPY_SOURCE) { auto b = Transition(r.res.Get(), r.state, D3D12_RESOURCE_STATE_COPY_SOURCE); ProbedBarrier(1, &b, __LINE__); r.state = D3D12_RESOURCE_STATE_COPY_SOURCE; }
        UINT rowPitch = (rw * 4 + 255) & ~255u;
        D3D12_TEXTURE_COPY_LOCATION dst{ g_readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; dst.PlacedFootprint.Footprint = { DXGI_FORMAT_B8G8R8A8_UNORM, rw, rh, 1, rowPitch };
        D3D12_TEXTURE_COPY_LOCATION sl{ r.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; sl.SubresourceIndex = 0;
        EnsureReadback();   // sesion 19: sin resolves por CPU no existia y las capturas salian negras
        dst.pResource = g_readback.Get();
        if (g_readback)
        {
            g_list->CopyTextureRegion(&dst, 0, 0, 0, &sl, nullptr);
            FlushAndWait();
            uint8_t* p = nullptr; g_readback->Map(0, nullptr, (void**)&p);
            if (p)
            {
                readback->resize(size_t(rw) * rh);
                for (UINT y = 0; y < rh; y++)
                    for (UINT x = 0; x < rw; x++)
                    {
                        const uint8_t* px = p + y * rowPitch + x * 4;   // B G R A del RT
                        // Igual que el camino por memoria: con swapRB la imagen sale con los colores del RT; sin el, con R y B cambiados
                        uint32_t b = r.swapRB ? px[0] : px[2], g = px[1], rr = r.swapRB ? px[2] : px[0];
                        (*readback)[size_t(y) * rw + x] = b | (g << 8) | (rr << 16) | 0xFF000000u;
                    }
                g_readback->Unmap(0, nullptr);
            }
        }
    }
    if (r.state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) { auto b = Transition(r.res.Get(), r.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); ProbedBarrier(1, &b, __LINE__); r.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; }
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
    sd.Shader4ComponentMapping = r.swapRB ? D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0, 1, 2, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1)
                                          : D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(2, 1, 0, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1);
    g_dev->CreateShaderResourceView(r.res.Get(), &sd, SrvCpu(3 + fi));
    auto toRt = Transition(g_backbuffers[fi].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ProbedBarrier(1, &toRt, __LINE__);
    D3D12_CPU_DESCRIPTOR_HANDLE bbRtv = Rtv(fi);
    g_list->OMSetRenderTargets(1, &bbRtv, FALSE, nullptr);
    { const float black[4] = { 0, 0, 0, 1 }; g_list->ClearRenderTargetView(bbRtv, black, 0, nullptr); }
    PresentSides(SrvGpu(3 + fi));   // GoWJ sesion 11: laterales difuminados
    D3D12_VIEWPORT vp; D3D12_RECT sr; PresentViewport(vp, sr);
    g_list->RSSetViewports(1, &vp); g_list->RSSetScissorRects(1, &sr);
    g_list->SetPipelineState(g_blitPsoScene && PanoActive() ? g_blitPsoScene.Get() : g_blitPso.Get());
    g_list->SetGraphicsRootSignature(g_blitRootSig.Get());
    ID3D12DescriptorHeap* heaps[] = { g_srvHeap.Get() };
    g_list->SetDescriptorHeaps(1, heaps);
    g_list->SetGraphicsRootDescriptorTable(0, SrvGpu(3 + fi));
    g_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_list->DrawInstanced(3, 1, 0, 0);
    OverlayDraw(fi);   // sesion 25: contador (F3)
    auto toPresent = Transition(g_backbuffers[fi].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    ProbedBarrier(1, &toPresent, __LINE__);
    HRESULT phr = g_list->Close();
    if (SUCCEEDED(phr)) { ID3D12CommandList* lists[] = { g_list.Get() }; g_queue->ExecuteCommandLists(1, lists); }
    else { static int pn = 0; if (pn++ < 10) LOG("[d3d12] Close de la lista fallo al presentar (0x%08lX, draws %llu): se descarta", (unsigned long)phr, (unsigned long long)g_frameDraws); DumpCmdTrace(phr); }
    g_cmdTrace.clear();
    {
        // GOWJ_VSYNC=0: presentar sin esperar al vsync (el Present bloqueaba el hilo de comandos de la GPU hasta 16 ms)
        static const UINT syncInterval = getenv("GOWJ_VSYNC") ? (UINT)atoi(getenv("GOWJ_VSYNC")) : 1u;
        double tp = QpcMs(); g_swap->Present(syncInterval, 0); g_hPresentMs += QpcMs() - tp;
    }
    g_fenceValues[fi] = g_nextFence;
    g_queue->Signal(g_fence.Get(), g_nextFence++);
    g_frameOpen = false;
    ResizeSwapIfNeeded();
    {   // Tirones: intervalo entre presentaciones por encima del umbral, con el desglose de lo que costo ese frame
        static const double hitchMs = getenv("GOWJ_HITCHMS") ? atof(getenv("GOWJ_HITCHMS")) : 40.0;
        static double last = 0; static uint64_t hn = 0, hitches = 0; double now = QpcMs(); hn++;
        double dt = last > 0 ? now - last : 0; last = now;
        {   // Sesion 16 de NG2: fps medios cada 5 s ([fps]); GOWJ_NOFPSLOG=1 lo quita
            static const bool noFps = getenv("GOWJ_NOFPSLOG") != nullptr;
            static double winStart = 0; static uint64_t winFrames = 0; static double worst = 0;
            if (!noFps)
            {
                if (winStart == 0) winStart = now;
                winFrames++; if (dt > worst) worst = dt;
                if (now - winStart >= 5000)
                { LOG("[fps] %.1f fps (%.1f ms de media, peor %.0f ms)", winFrames * 1000.0 / (now - winStart), (now - winStart) / winFrames, worst); winStart = now; winFrames = 0; worst = 0; { extern void TexDirtyStatsLog(); TexDirtyStatsLog(); }
                  static int fpsLines = 0; if (++fpsLines % 12 == 0) { extern void GuestHeapStatsLog(); GuestHeapStatsLog(); } }   // sesion 23: memoria cada minuto
            }
        }
        {   // GOWJ_FRAMELOG=<desde>:<cuantos>: draws y tiempo de cada frame presentado (ritmo de la escena 3D, sesion 24)
            static long long flFrom = -1, flCount = 0;
            static bool flInit = false;
            if (!flInit) { flInit = true; if (const char* s = getenv("GOWJ_FRAMELOG")) { flFrom = atoll(s); if (const char* c = strchr(s, ':')) flCount = atoll(c + 1); } }
            if (flFrom >= 0 && (long long)hn >= flFrom && (long long)hn < flFrom + flCount)
                LOG("[framelog] %llu: %.1f ms, draws %llu (%.1f ms), present %.1f ms, espera GPU %.1f ms", (unsigned long long)hn, dt, (unsigned long long)g_frameDraws, g_hDrawMs, g_hPresentMs, g_hWaitMs);
        }
        { extern void HitchProfNotify(double, double, uint64_t); if (dt > 0) HitchProfNotify(now - dt, now, hn); }
        OverlayTick(g_frameDraws);
        g_framesNoScene = g_frameHasMovie ? 1000 : g_frameHasScene ? 0 : std::min(g_framesNoScene + 1, 1000); g_frameHasScene = false; g_frameHasMovie = false; { static int lastPano = -1, np = 0; int pa = PanoActive() ? 1 : 0; if (pa != lastPano && np++ < 40) LOG("[pano] presentacion %s en el frame %llu", pa ? "panoramica" : "16:9 (video o menu)", (unsigned long long)g_frameCounter); lastPano = pa; }   // sesion 27
        if (dt > hitchMs && hitches++ < 2000)
            LOG("[tiron] frame %llu: %.0f ms | compilar %.0f ms (%u nuevos, %u de cache) | PSO %.0f ms (%u) | texturas %.0f ms (crear %.0f ms x%u, subir %.0f ms x%u) | espera GPU %.0f ms | vaciados %.0f ms (%u)",
                (unsigned long long)hn, dt, g_hCompileMs, g_hCompiles, g_hCacheHits, g_hPsoMs, g_hPsos, g_hTexMs, g_hTexCreateMs, g_hTexNew, g_hTexFillMs, g_hTexUp, g_hWaitMs, g_hFlushMs, g_hFlushes);
        g_hCompileMs = g_hPsoMs = g_hTexMs = g_hWaitMs = g_hFlushMs = g_hDrawMs = g_hPresentMs = g_hTexCreateMs = g_hTexFillMs = 0; g_hTexNew = g_hTexUp = 0; g_hCompiles = g_hCacheHits = g_hPsos = g_hFlushes = 0;
    }
    static uint64_t n = 0;
    if (++n % 120 == 0)
    {
        LOG("[d3d12] presentacion directa %llu: resolves %llu (por GPU %llu) | por frame: texturas subidas %.1f (%.1f MB), vertices copiados %.1f MB",
            (unsigned long long)n, (unsigned long long)g_resolves, (unsigned long long)g_gpuResolves, g_texUploads / 120.0, g_texUploadBytes / 120.0 / 1048576.0, g_vbCopyBytes / 120.0 / 1048576.0);
        g_texUploads = g_texUploadBytes = g_vbCopyBytes = 0;
        LOG("[d3d12] por frame: resolves por GPU %.1f, por CPU %.1f, volcados GPU->memoria %.1f", g_gpuResolvesWin / 120.0, g_cpuResolvesWin / 120.0, g_writeBacks / 120.0);
        g_gpuResolvesWin = g_cpuResolvesWin = g_writeBacks = 0;
        if (g_flushTotal) LOG("[d3d12] resolves por CPU diferidos: %.1f por frame en %.1f esperas (%.1f por solape con texturas/vertices)", g_deferredCount / 120.0, g_flushTotal / 120.0, g_flushOverlap / 120.0);
        g_deferredCount = g_flushTotal = g_flushOverlap = 0;
    }
    return true;
}

bool D3D12Present(HWND hwnd, const uint32_t* pixels, uint32_t w, uint32_t h)
{
    if (g_dev) MemexFlushPending();   // GoWJ sesion 11
    if (g_failed || !hwnd) return false;
    if (!g_dev && !Init(hwnd)) return false;
    OpenFrame();
    UINT fi = g_fi;
    static bool showEdram = getenv("GOWJ_SHOWEDRAM") != nullptr;
    bool useEdram = showEdram && g_frameDraws > 0;
    auto toRt = Transition(g_backbuffers[fi].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ProbedBarrier(1, &toRt, __LINE__);
    const float clear[4] = { 0.06f, 0.06f, 0.09f, 1.0f };
    g_list->ClearRenderTargetView(Rtv(fi), clear, 0, nullptr);
    UINT srvSlot = 0; bool blit = false;
    if (useEdram)
    {
        auto b = Transition(g_edram.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        ProbedBarrier(1, &b, __LINE__);
        srvSlot = 17; blit = true;
    }
    else if (pixels && w && h)
    {
        if (!g_fbTexture || g_fbW != w || g_fbH != h)
        {
            g_fbW = w; g_fbH = h;
            D3D12_HEAP_PROPERTIES def{ D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
            g_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&g_fbTexture));
            g_dev->CreateShaderResourceView(g_fbTexture.Get(), nullptr, SrvCpu(0));
        }
        UINT64 pitch = (UINT64(w) * 4 + 255) & ~255ull;
        UINT64 o = RingAlloc(pitch * h, 512);
        if (o != ~0ull)
        {
            for (uint32_t y = 0; y < h; y++) memcpy(g_ringMap[fi] + o + y * pitch, pixels + size_t(y) * w, size_t(w) * 4);
            D3D12_TEXTURE_COPY_LOCATION dst{ g_fbTexture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; dst.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION src{ g_ring[fi].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
            src.PlacedFootprint.Offset = o; src.PlacedFootprint.Footprint = { DXGI_FORMAT_B8G8R8A8_UNORM, w, h, 1, (UINT)pitch };
            auto a = Transition(g_fbTexture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            ProbedBarrier(1, &a, __LINE__);
            g_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            auto b = Transition(g_fbTexture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            ProbedBarrier(1, &b, __LINE__);
            srvSlot = 0; blit = true;
        }
    }
    if (blit)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE bbRtv = Rtv(fi);
        g_list->OMSetRenderTargets(1, &bbRtv, FALSE, nullptr);
        PresentSides(SrvGpu(srvSlot));   // GoWJ sesion 11: laterales difuminados
        D3D12_VIEWPORT vp; D3D12_RECT sr; PresentViewport(vp, sr);
        g_list->RSSetViewports(1, &vp); g_list->RSSetScissorRects(1, &sr);
        g_list->SetPipelineState(g_blitPsoScene && PanoActive() ? g_blitPsoScene.Get() : g_blitPso.Get());
        g_list->SetGraphicsRootSignature(g_blitRootSig.Get());
        ID3D12DescriptorHeap* heaps[] = { g_srvHeap.Get() };
        g_list->SetDescriptorHeaps(1, heaps);
        g_list->SetGraphicsRootDescriptorTable(0, SrvGpu(srvSlot));
        g_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_list->DrawInstanced(3, 1, 0, 0);
    }
    if (useEdram) { auto b = Transition(g_edram.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET); ProbedBarrier(1, &b, __LINE__); }
    g_framesNoScene = g_frameHasMovie ? 1000 : g_frameHasScene ? 0 : std::min(g_framesNoScene + 1, 1000); g_frameHasScene = false; g_frameHasMovie = false; { static int lastPano = -1, np = 0; int pa = PanoActive() ? 1 : 0; if (pa != lastPano && np++ < 40) LOG("[pano] presentacion %s en el frame %llu", pa ? "panoramica" : "16:9 (video o menu)", (unsigned long long)g_frameCounter); lastPano = pa; }   // sesion 27: presentacion desde memoria (videos)
    OverlayDraw(fi);   // sesion 25: contador (F3)
    auto toPresent = Transition(g_backbuffers[fi].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    ProbedBarrier(1, &toPresent, __LINE__);
    HRESULT phr = g_list->Close();
    if (SUCCEEDED(phr)) { ID3D12CommandList* lists[] = { g_list.Get() }; g_queue->ExecuteCommandLists(1, lists); }
    else { static int pn = 0; if (pn++ < 10) LOG("[d3d12] Close de la lista fallo al presentar (0x%08lX, draws %llu): se descarta", (unsigned long)phr, (unsigned long long)g_frameDraws); DumpCmdTrace(phr); }
    g_cmdTrace.clear();
    HRESULT hr = g_swap->Present(1, 0);
    g_fenceValues[fi] = g_nextFence;
    g_queue->Signal(g_fence.Get(), g_nextFence++);
    g_frameOpen = false;
    ResizeSwapIfNeeded();
    static uint64_t frames = 0;
    static long long dumpAt = getenv("GOWJ_DUMPFRAME") ? atoll(getenv("GOWJ_DUMPFRAME")) : -1;
    ++frames;
    if ((long long)frames == dumpAt) { g_fence->SetEventOnCompletion(g_nextFence - 1, g_fenceEvent); WaitForSingleObject(g_fenceEvent, 5000); DumpEdram(frames); }
    if (frames % 600 == 0) SavePsoLib();
    { static double lastMem = QpcMs(); if (QpcMs() - lastMem > 60000) { lastMem = QpcMs(); extern void GuestHeapStatsLog(); GuestHeapStatsLog(); } }   // sesion 23: cada minuto   // sesion 23: guarda los PSO nuevos que queden pendientes
    if (frames % 120 == 0) LOG("[d3d12] frame %llu: draws %llu, omitidos prim=%llu shader=%llu pso=%llu mem=%llu, shaders %zu, psos %zu, resolves %llu (frontbuffer %llu), superficies %zu",
        (unsigned long long)frames, (unsigned long long)g_frameDraws, (unsigned long long)g_skipped[0], (unsigned long long)g_skipped[1],
        (unsigned long long)g_skipped[2], (unsigned long long)g_skipped[3], g_shaders.size(), g_psos.size(), (unsigned long long)g_resolves, (unsigned long long)g_fbResolves, g_edramRts.size());
    if (FAILED(hr)) { LOG("[d3d12] Present fallo (0x%08lX)", (unsigned long)hr); g_failed = true; return false; }
    return true;
}
