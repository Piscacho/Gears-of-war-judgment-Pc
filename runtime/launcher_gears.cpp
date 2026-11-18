// Launcher de Gears of War: Judgment (sesion 9). Ventana de configuracion que se abre antes del juego, portada del launcher del port
// de Dead Space: Extraction (mismo autor): todo dibujado con GDI en un bufer, barra lateral con paginas, filas "< valor >", conmutadores
// SI/NO, campos de texto y botones GUARDAR / JUGAR. Estetica de Gears: acero oscuro, rojo carmesi y lineas de escaneo.
// Lee y escribe gowj.ini conservando comentarios y claves desconocidas (launcher.cpp las aplica al arrancar el juego).
// Imagenes opcionales en launcher\ junto al exe: portada.(jpg|png|bmp) (cabecera de Inicio y fondo) y logo.png (barra lateral).
// "mostrar_launcher = 0" entra directo al juego (mantener Mayus al abrir lo vuelve a mostrar); --jugar lo salta y --launcher lo fuerza.
#include "runtime.h"
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <commdlg.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "msimg32.lib")

std::string FindTitleUpdate(const std::string& dataDir);   // launcher.cpp

namespace
{
// --- Aspecto -------------------------------------------------------------------------------
const COLORREF kBg = RGB(8, 8, 9);
const COLORREF kPanel = RGB(20, 20, 22);
const COLORREF kPanelHi = RGB(38, 22, 22);
const COLORREF kLine = RGB(78, 70, 68);
const COLORREF kRed = RGB(205, 38, 32);
const COLORREF kRedHot = RGB(240, 70, 55);
const COLORREF kRedDim = RGB(140, 40, 36);
const COLORREF kText = RGB(232, 228, 222);
const COLORREF kMuted = RGB(150, 142, 136);
const COLORREF kPlayText = RGB(250, 244, 238);

const wchar_t* kVersion = L"1.0";
const wchar_t* kAuthor = L"Piscacho";

enum Page { kHome, kGraphics, kControls, kAudio, kOnline, kAdvanced, kPages };
const wchar_t* kPageNames[kPages] = { L"INICIO", L"GRÁFICOS", L"CONTROLES", L"AUDIO", L"EN LÍNEA", L"AVANZADO" };

enum : int
{
    kIdPlay = 1, kIdSave, kIdBrowseIso, kIdBrowseData, kIdOpenLogs, kIdOpenSaves, kIdOpenDlc,
    kIdPageBase = 100, kIdChoiceBase = 1000, kIdToggleBase = 2000, kIdTextBase = 5000,
};

struct Choice
{
    const char* key;
    const wchar_t* label;
    const wchar_t* hint;
    std::vector<std::pair<std::string, std::wstring>> options;
    const char* fallback;
};
struct Toggle { const char* key; const wchar_t* label; const wchar_t* hint; bool fallback; };
struct TextSetting { const char* key; const wchar_t* label; const wchar_t* hint; const char* fallback; };

const Choice kChoices[] = {
    { "fps", L"Fotogramas por segundo", L"El juego va a 30 en la consola. Más alto suaviza menús y escenas ligeras.",
        { { "30", L"30 (original)" }, { "60", L"60" }, { "120", L"120" }, { "144", L"144" }, { "sin_limite", L"Sin límite" } }, "30" },
    { "panoramico", L"Relación de aspecto", L"21:9 y 32:9 amplían la vista a los lados (Hor+); el HUD queda en proporción.",
        { { "16:9", L"16:9 (original)" }, { "21:9", L"21:9 ultrapanorámico" }, { "32:9", L"32:9 superultrapanorámico" },
          { "auto", L"Automático (monitor)" } }, "16:9" },
    { "ventana", L"Tamaño de la ventana", L"En modo ventana. F11 o Alt+Intro alternan la pantalla completa.",
        { { "1280x720", L"1280 x 720" }, { "1600x900", L"1600 x 900" }, { "1920x1080", L"1920 x 1080" }, { "2560x1440", L"2560 x 1440" },
          { "3840x2160", L"3840 x 2160" }, { "2560x1080", L"2560 x 1080 (21:9)" }, { "3440x1440", L"3440 x 1440 (21:9)" },
          { "3840x1080", L"3840 x 1080 (32:9)" }, { "5120x1440", L"5120 x 1440 (32:9)" } }, "1280x720" },
    { "anisotropico", L"Filtrado anisótropo", L"Texturas más nítidas en ángulo (suelos, paredes).",
        { { "0", L"Desactivado" }, { "2", L"2x" }, { "4", L"4x" }, { "8", L"8x" }, { "16", L"16x" } }, "16" },
    { "escalado", L"Escalado de la imagen", L"Cómo se agranda la imagen de 720p a la ventana.",
        { { "catmull-rom", L"Nítido (Catmull-Rom)" }, { "bilineal", L"Suave (bilineal)" } }, "catmull-rom" },
    { "latencia_audio", L"Latencia del audio", L"Baja responde antes; segura evita cortes en PCs cargados.",
        { { "baja", L"Baja (25 ms)" }, { "normal", L"Normal (40 ms)" }, { "segura", L"Segura (80 ms)" } }, "normal" },
    { "online", L"Modo", L"Partida por enlace de sistema (LAN) entre PCs, directa (P2P).",
        { { "no", L"Desactivado" }, { "crear", L"Crear partida (anfitrión)" }, { "unirse", L"Unirse a una partida" } }, "no" },
    { "idioma", L"Idioma", L"Textos y voces. Español = latino con doblaje. Los subtítulos se activan en Opciones del juego.",
        { { "es", L"Español (latino)" }, { "en", L"English" }, { "fr", L"Français" }, { "de", L"Deutsch" }, { "it", L"Italiano" },
          { "pl", L"Polski" }, { "ru", L"Русский" }, { "ko", L"한국어" }, { "zh", L"中文" } }, "es" },
    { "sens_raton", L"Sensibilidad del ratón", L"Velocidad de la cámara con el ratón. Clic en la ventana lo captura; Alt lo suelta.",
        { { "0.5", L"0,5" }, { "0.75", L"0,75" }, { "1", L"1 (normal)" }, { "1.5", L"1,5" }, { "2", L"2" }, { "3", L"3" } }, "1" },
    { "nitidez", L"Nitidez", L"Enfoque adaptativo al agrandar la imagen (compensa el suavizado del juego).",
        { { "0", L"Desactivada" }, { "0.25", L"Baja" }, { "0.5", L"Media" }, { "0.75", L"Alta" }, { "1", L"Máxima" } }, "0.5" },
    { "brillo", L"Brillo", L"Gamma final de la imagen (1,0 = como la consola).",
        { { "0.8", L"0,8 (más oscuro)" }, { "0.9", L"0,9" }, { "1.0", L"1,0 (consola)" }, { "1.1", L"1,1" }, { "1.2", L"1,2 (más claro)" } }, "1.0" },
    { "resolucion", L"Resolución interna", L"Dibuja el juego a más resolución (más nítido en monitores grandes; cuesta rendimiento).",
        { { "1", L"1x (720p, original)" }, { "2", L"2x (1440p)" }, { "3", L"3x (2160p)" } }, "1" },
    { "bordes", L"Bordes en vídeos y menús", L"Con monitor ancho, las pantallas 16:9 llevan los lados difuminados o barras negras.",
        { { "difuminado", L"Difuminados" }, { "negro", L"Barras negras" } }, "difuminado" },
};
enum { kChoiceFps, kChoiceAspect, kChoiceWindow, kChoiceAniso, kChoiceScale, kChoiceAudioLat, kChoiceOnline, kChoiceLang, kChoiceMouse, kChoiceSharp, kChoiceBright, kChoiceRes, kChoiceBorders, kChoiceCount };

const TextSetting kTexts[] = {
    { "online_direccion", L"Dirección del anfitrión", L"La IP que le aparece al que crea la partida (o la de su VPN).", "" },
    { "online_puerto", L"Puerto UDP", L"El anfitrión lo abre en su router o usa una VPN (Radmin, ZeroTier).", "3074" },
    { "online_nombre", L"Nombre del jugador", L"El nombre con el que te ven los demás.", "Jugador" },
};
enum { kTextAddress, kTextPort, kTextName, kTextCount };

const Toggle kToggles[] = {
    { "pantalla_completa", L"Pantalla completa", L"Sin bordes, en el monitor de la ventana.", false },
    { "vsync", L"Sincronización vertical (VSync)", L"Evita el desgarro de la imagen.", true },
    { "mostrar_fps", L"Contador de FPS", L"FPS, tiempo por cuadro, CPU y memoria. F3 lo muestra y oculta.", false },
    { "desenfoque_movimiento", L"Desenfoque de movimiento", L"Desenfoque de movimiento y radial como en la consola (aquí emborrona a los personajes).", false },
    { "grano_pelicula", L"Grano de película", L"El ruido fino del último pase de la consola.", false },
    { "vibracion", L"Vibración del mando", L"El mando vibra con los disparos y golpes.", true },
    { "saltar_intro", L"Saltar los vídeos de inicio", L"Entra directo a la pantalla de título.", false },
    { "perfil", L"Guardar partidas", L"Con perfil se guardan la campaña y los ajustes.", true },
    { "actualizacion", L"Usar la actualización del título", L"Si está instalada en la carpeta de datos.", true },
    { "mostrar_launcher", L"Mostrar este launcher al abrir", L"Si se desactiva, mantén Mayús al abrir para verlo.", true },
    { "invertir_y", L"Invertir el eje vertical del ratón", L"Mover el ratón hacia arriba mira hacia abajo.", false },
};
enum { kTogFull, kTogVsync, kTogFps, kTogMblur, kTogGrain, kTogRumble, kTogSkipIntro, kTogProfile, kTogTu, kTogLauncher, kTogInvert, kToggleCount };

// --- gowj.ini como lista de lineas ----------------------------------------------------------
struct IniFile
{
    std::vector<std::string> lines; std::string path;
    static std::string Trim(const std::string& s) { size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t\r\n"); return a == std::string::npos ? "" : s.substr(a, b - a + 1); }
    void Load(const std::string& f)
    {
        path = f; lines.clear();
        if (FILE* fp = fopen(f.c_str(), "r")) { char b[2048]; while (fgets(b, sizeof(b), fp)) { std::string s = b; while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back(); lines.push_back(s); } fclose(fp); }
    }
    std::string Get(const std::string& key, const std::string& fallback) const
    {
        for (const auto& l : lines) { std::string t = Trim(l); if (t.empty() || t[0] == '#' || t[0] == ';') continue; size_t e = t.find('='); if (e != std::string::npos && Trim(t.substr(0, e)) == key) return Trim(t.substr(e + 1)); }
        return fallback;
    }
    void Set(const std::string& key, const std::string& value)
    {
        for (auto& l : lines) { std::string t = Trim(l); if (t.empty() || t[0] == '#' || t[0] == ';') continue; size_t e = t.find('='); if (e != std::string::npos && Trim(t.substr(0, e)) == key) { l = key + "=" + value; return; } }
        lines.push_back(key + "=" + value);
    }
    bool Save() const
    {
        FILE* fp = fopen(path.c_str(), "w"); if (!fp) return false;
        for (const auto& l : lines) fprintf(fp, "%s\n", l.c_str());
        fclose(fp); return true;
    }
};

struct Hit { RECT r; int id; };
struct State
{
    HWND wnd = nullptr; int dpi = 96; IniFile ini; Page page = kHome;
    std::vector<Hit> hits; int hover = 0; bool play = false, done = false, dirty = false; int editing = -1;
    std::wstring status; IWICImagingFactory* wic = nullptr;
    HFONT title = nullptr, subtitle = nullptr, heading = nullptr, body = nullptr, smallFont = nullptr, button = nullptr, playFont = nullptr;
} g;

int S(int v) { return MulDiv(v, g.dpi, 96); }
std::string ExeDirG() { char p[MAX_PATH]; GetModuleFileNameA(nullptr, p, MAX_PATH); std::string s = p; size_t k = s.find_last_of("\\/"); return k == std::string::npos ? "." : s.substr(0, k); }
std::wstring Wide(const std::string& s)
{
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0); std::wstring w(size_t(std::max(0, n - 1)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n); return w;
}
std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr); std::string s(size_t(std::max(0, n - 1)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr); return s;
}
std::string IniValue(const char* key, const char* fallback) { return g.ini.Get(key, fallback); }
void SetIni(const char* key, const std::string& value) { g.ini.Set(key, value); g.dirty = true; }
int ChoiceIndex(int i)
{
    std::string v = IniValue(kChoices[i].key, kChoices[i].fallback);
    for (size_t k = 0; k < kChoices[i].options.size(); k++) if (kChoices[i].options[k].first == v) return int(k);
    return -1;
}
bool ToggleValue(int i) { std::string v = IniValue(kToggles[i].key, kToggles[i].fallback ? "1" : "0"); return v == "1" || v == "si" || v == "true"; }
std::string DataDir() { std::string d = IniValue("datos", "userdata"); return (d.size() > 1 && d[1] == ':') ? d : ExeDirG() + "\\" + d; }

// --- Dibujo ----------------------------------------------------------------------------------
HFONT MakeFont(int size, int weight, const wchar_t* face)
{ return CreateFontW(-S(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face); }
void FillRectColor(HDC dc, RECT r, COLORREF c) { HBRUSH b = CreateSolidBrush(c); FillRect(dc, &r, b); DeleteObject(b); }
// Panel con esquinas cortadas (arriba a la izquierda y abajo a la derecha)
void Chamfer(HDC dc, RECT r, int cut, COLORREF fill, COLORREF border, int width = 1)
{
    POINT pts[6] = { { r.left + cut, r.top }, { r.right, r.top }, { r.right, r.bottom - cut }, { r.right - cut, r.bottom }, { r.left, r.bottom }, { r.left, r.top + cut } };
    HBRUSH b = fill == CLR_NONE ? (HBRUSH)GetStockObject(NULL_BRUSH) : CreateSolidBrush(fill);
    HPEN p = border == CLR_NONE ? (HPEN)GetStockObject(NULL_PEN) : CreatePen(PS_SOLID, width, border);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    Polygon(dc, pts, 6);
    SelectObject(dc, ob); SelectObject(dc, op);
    if (fill != CLR_NONE) DeleteObject(b);
    if (border != CLR_NONE) DeleteObject(p);
}
void Line(HDC dc, int x0, int y0, int x1, int y1, COLORREF c, int width = 1)
{ HPEN p = CreatePen(PS_SOLID, width, c); HGDIOBJ op = SelectObject(dc, p); MoveToEx(dc, x0, y0, nullptr); LineTo(dc, x1, y1); SelectObject(dc, op); DeleteObject(p); }
void Text(HDC dc, const std::wstring& t, RECT r, COLORREF c, HFONT f, UINT fmt = DT_LEFT | DT_VCENTER | DT_SINGLELINE, int spacing = 0)
{
    SetTextColor(dc, c); SetBkMode(dc, TRANSPARENT); HGDIOBJ of = SelectObject(dc, f);
    SetTextCharacterExtra(dc, spacing); DrawTextW(dc, t.c_str(), -1, &r, fmt | DT_NOPREFIX); SetTextCharacterExtra(dc, 0); SelectObject(dc, of);
}
void FillAlpha(HDC dc, RECT r, COLORREF c, int alpha, int cut = 0)
{
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = 1; bi.bmiHeader.biHeight = 1; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr; HBITMAP px = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!px) return;
    *(DWORD*)bits = 0xFF000000u | (DWORD(GetRValue(c)) << 16) | (DWORD(GetGValue(c)) << 8) | GetBValue(c);
    HRGN rgn = nullptr;
    if (cut > 0)
    {
        POINT pts[6] = { { r.left + cut, r.top }, { r.right, r.top }, { r.right, r.bottom - cut }, { r.right - cut, r.bottom }, { r.left, r.bottom }, { r.left, r.top + cut } };
        rgn = CreatePolygonRgn(pts, 6, WINDING); SelectClipRgn(dc, rgn);
    }
    HDC mem = CreateCompatibleDC(dc); HGDIOBJ old = SelectObject(mem, px);
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, BYTE(alpha), 0 };
    AlphaBlend(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, mem, 0, 0, 1, 1, bf);
    SelectObject(mem, old); DeleteDC(mem); DeleteObject(px);
    if (rgn) { SelectClipRgn(dc, nullptr); DeleteObject(rgn); }
}

// --- Imagenes (WIC; se escalan una vez por tamano) -------------------------------------------
// Imagenes de SteamGridDB (launcher\CREDITOS.txt): arte de la cabecera, dos heroes para los fondos, logo y caratula
enum Picture { kPicArt, kPicBaird, kPicSquad, kPicLogo, kPicCover, kPictures };
struct PictureData { IWICBitmapSource* source = nullptr; UINT w = 0, h = 0; struct Scaled { RECT src; int w, h; HBITMAP bmp; }; std::vector<Scaled> cache; } g_pics[kPictures];
void LoadPictures()
{
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g.wic));
    if (FAILED(hr)) { LOG("[launcher] WIC no disponible (0x%08lX)", (unsigned long)hr); return; }
    const std::vector<std::vector<const char*>> files = { { "heroe_arte.jpg", "portada.jpg", "portada.png", "portada.bmp" }, { "heroe_baird.jpg" },
        { "heroe_escuadron.jpg" }, { "logo.png" }, { "portada_x360.jpg" } };
    for (int i = 0; i < kPictures; i++)
        for (const char* f : files[i])
        {
            std::wstring path = Wide(ExeDirG() + "\\launcher\\" + f);
            IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* frame = nullptr; IWICBitmapSource* conv = nullptr;
            if (SUCCEEDED(g.wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec)) &&
                SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(WICConvertBitmapSource(GUID_WICPixelFormat32bppPBGRA, frame, &conv)))
            {
                IWICBitmap* mem = nullptr;
                if (SUCCEEDED(g.wic->CreateBitmapFromSource(conv, WICBitmapCacheOnLoad, &mem))) { g_pics[i].source = mem; mem->GetSize(&g_pics[i].w, &g_pics[i].h); }
                conv->Release();
            }
            if (frame) frame->Release();
            if (dec) dec->Release();
            if (g_pics[i].source) { LOG("[launcher] imagen %s (%ux%u)", f, g_pics[i].w, g_pics[i].h); break; }
        }
}
void FreePictures()
{
    for (PictureData& p : g_pics) { for (auto& c : p.cache) DeleteObject(c.bmp); p.cache.clear(); if (p.source) p.source->Release(); p.source = nullptr; }
    if (g.wic) g.wic->Release(); g.wic = nullptr;
}
HBITMAP ScaledPicture(int pic, RECT src, int w, int h)
{
    PictureData& p = g_pics[pic];
    if (!p.source || w <= 0 || h <= 0) return nullptr;
    for (auto& c : p.cache) if (EqualRect(&c.src, &src) && c.w == w && c.h == h) return c.bmp;
    if (p.cache.size() > 12) { DeleteObject(p.cache.front().bmp); p.cache.erase(p.cache.begin()); }
    IWICBitmapClipper* clip = nullptr; IWICBitmapScaler* scaler = nullptr; HBITMAP bmp = nullptr;
    WICRect wr{ src.left, src.top, src.right - src.left, src.bottom - src.top };
    if (SUCCEEDED(g.wic->CreateBitmapClipper(&clip)) && SUCCEEDED(clip->Initialize(p.source, &wr)) && SUCCEEDED(g.wic->CreateBitmapScaler(&scaler)) &&
        SUCCEEDED(scaler->Initialize(clip, UINT(w), UINT(h), WICBitmapInterpolationModeFant)))
    {
        BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr; bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bmp && FAILED(scaler->CopyPixels(nullptr, UINT(w) * 4, UINT(w) * UINT(h) * 4, (BYTE*)bits))) { DeleteObject(bmp); bmp = nullptr; }
    }
    if (scaler) scaler->Release();
    if (clip) clip->Release();
    if (bmp) p.cache.push_back({ src, w, h, bmp });
    return bmp;
}
void DrawPicture(HDC dc, int pic, RECT dst, RECT src, int alpha = 255)
{
    int w = dst.right - dst.left, h = dst.bottom - dst.top; HBITMAP bmp = ScaledPicture(pic, src, w, h);
    if (!bmp) return;
    HDC mem = CreateCompatibleDC(dc); HGDIOBJ old = SelectObject(mem, bmp);
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, BYTE(alpha), AC_SRC_ALPHA };
    AlphaBlend(dc, dst.left, dst.top, w, h, mem, 0, 0, w, h, bf);
    SelectObject(mem, old); DeleteDC(mem);
}
void DrawPictureCover(HDC dc, int pic, RECT dst, float fx = 0.5f, float fy = 0.5f, int alpha = 255)
{
    const PictureData& p = g_pics[pic]; if (!p.source) return;
    int w = dst.right - dst.left, h = dst.bottom - dst.top; double scale = std::max(double(w) / p.w, double(h) / p.h);
    int sw = std::min(int(p.w), int(w / scale + 0.5)), sh = std::min(int(p.h), int(h / scale + 0.5));
    int sx = int((p.w - sw) * fx), sy = int((p.h - sh) * fy);
    DrawPicture(dc, pic, dst, RECT{ sx, sy, sx + sw, sy + sh }, alpha);
}
int DrawPictureWidth(HDC dc, int pic, int x, int y, int w, int alpha = 255)
{
    const PictureData& p = g_pics[pic]; if (!p.source) return 0;
    int h = int(double(w) * p.h / p.w + 0.5); DrawPicture(dc, pic, RECT{ x, y, x + w, y + h }, RECT{ 0, 0, LONG(p.w), LONG(p.h) }, alpha); return h;
}
bool HasPicture(int pic) { return g_pics[pic].source != nullptr; }

void AddHit(RECT r, int id) { g.hits.push_back({ r, id }); }
bool Hovered(int id) { return g.hover == id; }
void Button(HDC dc, RECT r, const std::wstring& text, int id, bool primary = false)
{
    bool hot = Hovered(id);
    if (primary) Chamfer(dc, r, S(10), hot ? kRedHot : kRed, CLR_NONE);
    else Chamfer(dc, r, S(8), hot ? kPanelHi : kPanel, hot ? kRed : kLine);
    Text(dc, text, r, primary ? kPlayText : (hot ? kRedHot : kText), primary ? g.playFont : g.button, DT_CENTER | DT_VCENTER | DT_SINGLELINE, primary ? S(3) : S(1));
    AddHit(r, id);
}
// Titulo de seccion: mayusculas espaciadas en rojo con una regla y tres marcas
int Section(HDC dc, int x, int y, int w, const std::wstring& t)
{
    RECT r{ x, y, x + w, y + S(26) };
    Text(dc, t, r, kRed, g.heading, DT_LEFT | DT_VCENTER | DT_SINGLELINE, S(2));
    Line(dc, x, y + S(30), x + w, y + S(30), kLine);
    for (int k = 0; k < 3; k++) Line(dc, x + w - S(6) - k * S(8), y + S(27), x + w - S(10) - k * S(8), y + S(33), kRedDim);
    return y + S(44);
}
void RowLabel(HDC dc, int x, int y, int w, const wchar_t* label, const wchar_t* hint)
{
    RECT lr{ x, y, x + w / 2, y + S(22) }; Text(dc, label, lr, kText, g.body);
    if (hint && *hint) { RECT hr{ x, y + S(22), x + w - S(270), y + S(40) }; Text(dc, hint, hr, kMuted, g.smallFont, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS); }
}
void ChoiceRow(HDC dc, int x, int& y, int w, int i)
{
    const Choice& c = kChoices[i]; RowLabel(dc, x, y, w, c.label, c.hint);
    int cw = S(250), cx = x + w - cw; RECT box{ cx, y, cx + cw, y + S(34) };
    Chamfer(dc, box, S(6), kPanel, kLine);
    int idx = ChoiceIndex(i);
    std::wstring text = idx >= 0 ? c.options[size_t(idx)].second : Wide(IniValue(c.key, c.fallback));
    RECT tr{ cx + S(34), y, cx + cw - S(34), y + S(34) }; Text(dc, text, tr, kText, g.body, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT left{ cx, y, cx + S(34), y + S(34) }, right{ cx + cw - S(34), y, cx + cw, y + S(34) };
    int lid = kIdChoiceBase + i * 2, rid = lid + 1;
    Text(dc, L"◀", left, Hovered(lid) ? kRedHot : kRedDim, g.body, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    Text(dc, L"▶", right, Hovered(rid) ? kRedHot : kRedDim, g.body, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    AddHit(left, lid); AddHit(right, rid);
    y += S(54);
}
void ToggleRow(HDC dc, int x, int& y, int w, int i)
{
    const Toggle& t = kToggles[i]; RowLabel(dc, x, y, w, t.label, t.hint);
    bool on = ToggleValue(i); int id = kIdToggleBase + i; int tw = S(84), tx = x + w - tw;
    RECT box{ tx, y + S(4), tx + tw, y + S(30) };
    Chamfer(dc, box, S(6), on ? kPanelHi : kPanel, on || Hovered(id) ? kRed : kLine);
    RECT knob = on ? RECT{ box.right - S(36), box.top + S(4), box.right - S(4), box.bottom - S(4) } : RECT{ box.left + S(4), box.top + S(4), box.left + S(36), box.bottom - S(4) };
    Chamfer(dc, knob, S(4), on ? kRed : kLine, CLR_NONE);
    RECT label = box; if (on) label.right -= S(38); else label.left += S(38);
    Text(dc, on ? L"SÍ" : L"NO", label, on ? kRedHot : kMuted, g.smallFont, DT_CENTER | DT_VCENTER | DT_SINGLELINE, S(1));
    AddHit(box, id);
    y += S(54);
}
void TextRow(HDC dc, int x, int& y, int w, int i)
{
    const TextSetting& t = kTexts[i]; RowLabel(dc, x, y, w, t.label, t.hint);
    int bw = S(250), bx = x + w - bw; RECT box{ bx, y, bx + bw, y + S(34) };
    int id = kIdTextBase + i; bool editing = g.editing == i;
    Chamfer(dc, box, S(6), editing ? kPanelHi : kPanel, editing || Hovered(id) ? kRed : kLine);
    std::wstring v = Wide(IniValue(t.key, t.fallback));
    if (editing && (GetTickCount64() / 500) % 2 == 0) v += L"|";
    RECT tr{ box.left + S(12), box.top, box.right - S(12), box.bottom };
    Text(dc, v.empty() ? L"(escribe aquí)" : v, tr, v.empty() ? kMuted : kText, g.body, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    AddHit(box, id);
    y += S(54);
}
// Una ruta (archivo o carpeta) con su boton ELEGIR...
void PathRow(HDC dc, int x, int& y, int w, const wchar_t* label, const wchar_t* hint, const std::string& value, int id)
{
    RowLabel(dc, x, y, w, label, hint); y += S(44);
    RECT path{ x, y, x + w - S(150), y + S(34) }; Chamfer(dc, path, S(6), kPanel, kLine);
    RECT pt{ path.left + S(12), path.top, path.right - S(12), path.bottom };
    std::wstring v = Wide(value);
    Text(dc, v.empty() ? L"(sin elegir)" : v, pt, v.empty() ? kMuted : kText, g.smallFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
    Button(dc, RECT{ x + w - S(140), y, x + w, y + S(34) }, L"ELEGIR…", id);
    y += S(52);
}

// --- Paginas ---------------------------------------------------------------------------------
void Banner(HDC dc, RECT banner)
{
    FillRectColor(dc, banner, RGB(0, 0, 0));
    if (HasPicture(kPicArt))
    {
        // El escuadron Kilo a la derecha; el logo sobre la izquierda (los Locust), oscurecida hacia el borde
        DrawPictureCover(dc, kPicArt, banner, 0.92f, 0.30f);
        int lw = (banner.right - banner.left) * 58 / 100;
        for (int i = 0; i < 12; i++) { RECT band{ banner.left + lw * i / 12, banner.top, banner.left + lw * (i + 1) / 12, banner.bottom }; FillAlpha(dc, band, RGB(4, 3, 3), 215 - i * 18); }
    }
    else
        for (int i = 0; i < 40; i++)   // sin imagen: degradado de rojo oscuro a negro
        { RECT band{ banner.left, banner.top + (banner.bottom - banner.top) * i / 40, banner.right, banner.top + (banner.bottom - banner.top) * (i + 1) / 40 }; FillRectColor(dc, band, RGB(40 - i * 3 / 4, 8, 8)); }
    if (HasPicture(kPicLogo))
    {
        int lw = (banner.right - banner.left) * 39 / 100, lh = int(double(lw) * g_pics[kPicLogo].h / g_pics[kPicLogo].w);
        DrawPictureWidth(dc, kPicLogo, banner.left + S(24), banner.top + (banner.bottom - banner.top - S(40) - lh) / 2, lw);
    }
    else
    {
        RECT t1{ banner.left + S(28), banner.top + S(36), banner.right, banner.top + S(96) };
        Text(dc, L"GEARS OF WAR", t1, kText, g.title, DT_LEFT | DT_VCENTER | DT_SINGLELINE, S(3));
        RECT t2{ banner.left + S(30), banner.top + S(96), banner.right, banner.top + S(130) };
        Text(dc, L"J U D G M E N T", t2, kRed, g.heading, DT_LEFT | DT_VCENTER | DT_SINGLELINE, S(4));
    }
    for (int yy = banner.top; yy < banner.bottom; yy += S(3)) FillAlpha(dc, RECT{ banner.left, yy, banner.right, yy + 1 }, RGB(0, 0, 0), 80);
    RECT band{ banner.left, banner.bottom - S(40), banner.right, banner.bottom };
    FillAlpha(dc, band, RGB(6, 6, 7), 210);
    Line(dc, banner.left, band.top, banner.right, band.top, kLine);
    Chamfer(dc, banner, S(14), CLR_NONE, kLine);
    RECT name{ band.left + S(20), band.top, band.right - S(34), band.bottom };
    Text(dc, L"HALVO BAY  ·  EL JUICIO DE KILO", name, kMuted, g.smallFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE, S(2));
    Text(dc, L"4D530A26  ·  DE XBOX 360 A PC", name, kMuted, g.smallFont, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}
void PaintHome(HDC dc, RECT area)
{
    int x = area.left, w = area.right - area.left, y = area.top;
    RECT banner{ x, y, area.right, y + S(240) }; Banner(dc, banner);
    y = banner.bottom + S(16);
    y = Section(dc, x, y, w, L"AJUSTES RÁPIDOS");
    ChoiceRow(dc, x, y, w, kChoiceFps);
    ChoiceRow(dc, x, y, w, kChoiceAspect);
    ToggleRow(dc, x, y, w, kTogFull);
    ChoiceRow(dc, x, y, w, kChoiceWindow);
    RECT card{ x, y - S(4), area.right, y + S(56) }; Chamfer(dc, card, S(10), kPanel, kLine);
    RECT c1{ card.left + S(16), card.top + S(4), card.right - S(16), card.top + S(28) };
    Text(dc, L"MANDO DE XBOX O DUALSENSE: como en la consola   ·   TECLADO Y RATÓN: WASD mover, ratón cámara", c1, kText, g.smallFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT c2{ card.left + S(16), card.top + S(28), card.right - S(16), card.top + S(52) };
    Text(dc, L"ESPACIO: A (cubrirse)   ·   Q: disparar   ·   Z: apuntar   ·   R: cambiar arma   ·   INTRO: pausa   ·   F11: pantalla completa", c2, kMuted, g.smallFont, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}
void PaintGraphics(HDC dc, RECT area)
{
    int x = area.left, w = area.right - area.left, y = area.top;
    y = Section(dc, x, y, w, L"IMAGEN");
    ChoiceRow(dc, x, y, w, kChoiceAspect);
    ChoiceRow(dc, x, y, w, kChoiceRes);   // GoWJ sesion 11
    ChoiceRow(dc, x, y, w, kChoiceAniso);
    ChoiceRow(dc, x, y, w, kChoiceScale);
    ChoiceRow(dc, x, y, w, kChoiceSharp);   // GoWJ sesion 11
    ChoiceRow(dc, x, y, w, kChoiceBright);   // GoWJ sesion 11
    ToggleRow(dc, x, y, w, kTogMblur);      // GoWJ sesion 11
    y = Section(dc, x, y + S(4), w, L"PANTALLA");   // GoWJ sesion 11: fps y ventana van en Inicio (no cabia todo)
    ToggleRow(dc, x, y, w, kTogFull);
    ToggleRow(dc, x, y, w, kTogVsync);
    ToggleRow(dc, x, y, w, kTogFps);
}
void PaintControls(HDC dc, RECT area)
{
    int x = area.left, w = area.right - area.left, y = area.top;
    y = Section(dc, x, y, w, L"CONTROLES");
    ToggleRow(dc, x, y, w, kTogRumble);
    ChoiceRow(dc, x, y, w, kChoiceMouse);
    ToggleRow(dc, x, y, w, kTogInvert);
    struct Row { const wchar_t* action; const wchar_t* pad; const wchar_t* keys; };
    static const Row rows[] = {
        { L"Moverse", L"Stick izquierdo", L"W A S D" }, { L"Cámara", L"Stick derecho", L"Ratón" },
        { L"Cubrirse / correr / acción", L"A", L"Espacio / Mayús" }, { L"Recargar (recarga activa)", L"RB", L"R" },
        { L"Apuntar", L"LT", L"Clic derecho" }, { L"Disparar", L"RT", L"Clic izquierdo" }, { L"Granada", L"LB", L"G" },
        { L"Cambiar de arma", L"Y / cruceta", L"Q / rueda / 1-4" }, { L"Golpe cuerpo a cuerpo", L"B", L"F / clic rueda" }, { L"Recoger / interactuar", L"X", L"E" },
        { L"Pausa", L"START", L"Esc / Intro" }, { L"Objetivos", L"BACK", L"Tab" },
    };
    int colPad = x + w - S(440), colKeys = x + w - S(200);
    RECT t1{ colPad, y - S(8), colPad + S(220), y + S(12) }, t2{ colKeys, y - S(8), colKeys + S(200), y + S(12) };
    Text(dc, L"MANDO", t1, kMuted, g.smallFont, DT_CENTER | DT_VCENTER | DT_SINGLELINE, S(1));
    Text(dc, L"TECLADO", t2, kMuted, g.smallFont, DT_CENTER | DT_VCENTER | DT_SINGLELINE, S(1));
    y += S(16);
    int row = S(28);
    for (int i = 0; i < int(sizeof(rows) / sizeof(rows[0])); i++)
    {
        int ry = y + i * row;
        if (i % 2 == 0) FillRectColor(dc, RECT{ x, ry, area.right, ry + row }, RGB(14, 14, 15));
        RECT lr{ x + S(10), ry, colPad - S(10), ry + row }; Text(dc, rows[i].action, lr, kText, g.body);
        RECT pr{ colPad, ry, colPad + S(220), ry + row }; Text(dc, rows[i].pad, pr, kText, g.smallFont, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT kr{ colKeys, ry, colKeys + S(200), ry + row }; Text(dc, rows[i].keys, kr, kText, g.smallFont, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}
void PaintAudio(HDC dc, RECT area)
{
    int x = area.left, w = area.right - area.left, y = area.top;
    y = Section(dc, x, y, w, L"IDIOMA Y SONIDO");
    ChoiceRow(dc, x, y, w, kChoiceLang);
    ChoiceRow(dc, x, y, w, kChoiceAudioLat);
    RECT card{ x, y + S(4), area.right, y + S(70) }; FillAlpha(dc, card, RGB(12, 12, 13), 200, S(10)); Chamfer(dc, card, S(10), CLR_NONE, kLine);
    RECT t{ card.left + S(16), card.top + S(10), card.right - S(16), card.bottom - S(10) };
    Text(dc, L"Los volúmenes de música, efectos y voces se ajustan en las opciones del propio juego (Opciones > Audio).", t, kMuted, g.smallFont, DT_LEFT | DT_TOP | DT_WORDBREAK);
}
void PaintOnline(HDC dc, RECT area)
{
    int x = area.left, w = area.right - area.left, y = area.top;
    y = Section(dc, x, y, w, L"JUEGO EN LÍNEA  ·  ENLACE DE SISTEMA P2P");
    ChoiceRow(dc, x, y, w, kChoiceOnline);
    std::string mode = IniValue("online", "no");
    if (mode == "unirse") TextRow(dc, x, y, w, kTextAddress);
    if (mode != "no") { TextRow(dc, x, y, w, kTextPort); TextRow(dc, x, y, w, kTextName); }
    RECT card{ x, y + S(4), area.right, y + S(4) + S(150) };
    FillAlpha(dc, card, RGB(12, 12, 13), 200, S(10)); Chamfer(dc, card, S(10), CLR_NONE, kLine);
    RECT t{ card.left + S(16), card.top + S(10), card.right - S(16), card.bottom - S(10) };
    const wchar_t* help =
        L"Cada PC ejecuta su copia del juego y el tráfico de red del juego viaja directo entre los PCs por UDP (sin Xbox Live). "
        L"El anfitrión elige \"Crear partida\" y crea una partida de enlace de sistema dentro del juego; los demás eligen "
        L"\"Unirse\", escriben su IP y buscan partidas de enlace de sistema.\n\nPor internet, el anfitrión debe abrir el puerto "
        L"UDP en su router; si no, usad una VPN como Radmin VPN o ZeroTier y la IP que os dé. Todos necesitáis el mismo port.";
    Text(dc, help, t, kMuted, g.smallFont, DT_LEFT | DT_TOP | DT_WORDBREAK);
}
void PaintAdvanced(HDC dc, RECT area)
{
    int x = area.left, w = area.right - area.left, y = area.top;
    y = Section(dc, x, y, w, L"JUEGO");
    PathRow(dc, x, y, w, L"ISO del juego", L"Gears of War: Judgment de Xbox 360 (tu copia).", IniValue("iso", ""), kIdBrowseIso);
    ToggleRow(dc, x, y, w, kTogSkipIntro);
    ToggleRow(dc, x, y, w, kTogProfile);
    ToggleRow(dc, x, y, w, kTogTu);
    ToggleRow(dc, x, y, w, kTogLauncher);
    ChoiceRow(dc, x, y, w, kChoiceBorders);   // GoWJ sesion 11 (en Graficos no cabia)
    y = Section(dc, x, y + S(4), w, L"CARPETAS");
    Button(dc, RECT{ x, y, x + S(220), y + S(36) }, L"PARTIDAS GUARDADAS", kIdOpenSaves);
    Button(dc, RECT{ x + S(236), y, x + S(456), y + S(36) }, L"CONTENIDO (DLC)", kIdOpenDlc);
    Button(dc, RECT{ x + S(472), y, x + S(692), y + S(36) }, L"REGISTRO (LOG)", kIdOpenLogs);
}

void Paint(HDC dc, RECT rc)
{
    g.hits.clear();
    FillRectColor(dc, rc, kBg);
    // Fondo: un arte distinto por pagina, oscurecido, bajo las lineas de escaneo
    static const int kPageArt[kPages] = { kPicBaird, kPicSquad, kPicArt, kPicBaird, kPicSquad, kPicArt };
    static const float kPageFx[kPages] = { 0.5f, 1.0f, 0.8f, 0.5f, 1.0f, 0.8f };   // el heroe del escuadron lleva su logo a la izquierda
    int art = kPageArt[g.page];
    if (HasPicture(art)) { DrawPictureCover(dc, art, rc, kPageFx[g.page], 0.35f); FillAlpha(dc, rc, RGB(5, 4, 4), 160); }
    for (int y = 0; y < rc.bottom; y += S(3)) FillAlpha(dc, RECT{ 0, y, rc.right, y + 1 }, RGB(0, 0, 0), 110);
    // Barra lateral
    int side = S(250);
    RECT sb{ 0, 0, side, rc.bottom }; FillAlpha(dc, sb, RGB(6, 6, 7), 220);
    Line(dc, side, 0, side, rc.bottom, kLine);
    if (HasPicture(kPicLogo)) DrawPictureWidth(dc, kPicLogo, S(14), S(30), side - S(28));
    else
    {
        RECT t1{ S(24), S(26), side - S(10), S(66) }; Text(dc, L"GEARS OF WAR", t1, kText, g.subtitle, DT_LEFT | DT_VCENTER | DT_SINGLELINE, S(2));
        RECT t2{ S(26), S(64), side - S(10), S(92) }; Text(dc, L"J U D G M E N T", t2, kRed, g.heading, DT_LEFT | DT_VCENTER | DT_SINGLELINE, S(2));
    }
    Line(dc, S(24), S(104), side - S(24), S(104), kLine);
    RECT t3{ S(24), S(110), side - S(10), S(134) }; Text(dc, L"PORT PC", t3, kRed, g.heading, DT_LEFT | DT_VCENTER | DT_SINGLELINE, S(4));
    RECT t4{ S(24), S(134), side - S(10), S(154) }; Text(dc, std::wstring(L"por ") + kAuthor + L"  ·  v" + kVersion, t4, kMuted, g.smallFont);
    int y = S(190);
    for (int p = 0; p < kPages; p++)
    {
        RECT r{ S(16), y, side - S(16), y + S(44) }; int id = kIdPageBase + p; bool active = g.page == p;
        if (active) Chamfer(dc, r, S(10), kPanelHi, kRed);
        else if (Hovered(id)) Chamfer(dc, r, S(10), kPanel, kLine);
        RECT tr{ r.left + S(22), r.top, r.right, r.bottom };
        Text(dc, kPageNames[p], tr, active ? kText : (Hovered(id) ? kRedHot : kMuted), g.button, DT_LEFT | DT_VCENTER | DT_SINGLELINE, S(2));
        if (active) Line(dc, r.left + S(10), r.top + S(14), r.left + S(10), r.bottom - S(14), kRed, S(3));
        AddHit(r, id); y += S(46);
    }
    // La caratula de Xbox 360 abajo en la barra lateral, si cabe
    if (HasPicture(kPicCover))
    {
        int coverW = S(112), coverH = int(double(coverW) * g_pics[kPicCover].h / g_pics[kPicCover].w);
        int cx = (side - coverW) / 2, cy = rc.bottom - S(74) - coverH;
        if (cy > y + S(10))
        {
            RECT frame{ cx - S(4), cy - S(4), cx + coverW + S(4), cy + coverH + S(4) };
            FillAlpha(dc, frame, RGB(0, 0, 0), 160);
            DrawPictureWidth(dc, kPicCover, cx, cy, coverW);
            Chamfer(dc, frame, S(8), CLR_NONE, kLine);
        }
    }
    RECT note{ S(24), rc.bottom - S(60), side - S(16), rc.bottom - S(14) };
    Text(dc, L"En el juego: F11 o Alt+Intro\npantalla completa  ·  F3 FPS.", note, kMuted, g.smallFont, DT_LEFT | DT_TOP | DT_WORDBREAK);
    // Panel de contenido
    RECT content{ side + S(30), S(26), rc.right - S(34), rc.bottom - S(96) };
    RECT panel{ content.left - S(14), content.top - S(10), content.right + S(14), content.bottom + S(10) };
    FillAlpha(dc, panel, RGB(10, 9, 9), 190, S(18)); Chamfer(dc, panel, S(18), CLR_NONE, kLine);
    // Las paginas con sitio de sobra terminan con una franja del arte de la pagina
    if ((g.page == kAudio || g.page == kOnline || g.page == kAdvanced) && HasPicture(art))
    {
        RECT strip{ content.left, content.bottom - S(170), content.right, content.bottom };
        int sart = art == kPicSquad && HasPicture(kPicBaird) ? kPicBaird : art;   // el del escuadron trae su logo: en franja se veria
        DrawPictureCover(dc, sart, strip, sart == kPicSquad ? 1.0f : (sart == kPicArt ? 0.8f : 0.5f), 0.35f, 235);
        for (int i = 0; i < 15; i++) { RECT band{ strip.left, strip.top + S(4) * i, strip.right, strip.top + S(4) * (i + 1) }; FillAlpha(dc, band, RGB(10, 9, 9), 230 - i * 15); }
        for (int yy = strip.top; yy < strip.bottom; yy += S(3)) FillAlpha(dc, RECT{ strip.left, yy, strip.right, yy + 1 }, RGB(0, 0, 0), 80);
        Chamfer(dc, strip, S(12), CLR_NONE, kLine);
    }
    switch (g.page)
    {
    case kHome: PaintHome(dc, content); break;
    case kGraphics: PaintGraphics(dc, content); break;
    case kControls: PaintControls(dc, content); break;
    case kAudio: PaintAudio(dc, content); break;
    case kOnline: PaintOnline(dc, content); break;
    default: PaintAdvanced(dc, content); break;
    }
    // Barra inferior
    RECT bar{ side, rc.bottom - S(72), rc.right, rc.bottom }; FillAlpha(dc, bar, RGB(6, 6, 7), 230);
    Line(dc, side, bar.top, rc.right, bar.top, kLine);
    RECT st{ side + S(30), bar.top, rc.right - S(420), bar.bottom };
    std::wstring status = g.status.empty() ? (g.dirty ? L"Cambios sin guardar." : L"Listo.") : g.status;
    Text(dc, status, st, g.dirty ? kRedHot : kMuted, g.smallFont);
    Button(dc, RECT{ rc.right - S(390), bar.top + S(16), rc.right - S(250), bar.bottom - S(16) }, L"GUARDAR", kIdSave);
    Button(dc, RECT{ rc.right - S(234), bar.top + S(12), rc.right - S(34), bar.bottom - S(12) }, L"JUGAR", kIdPlay, true);
}

// --- Acciones --------------------------------------------------------------------------------
bool Save()
{
    if (!g.ini.Save()) { g.status = L"No se pudo guardar gowj.ini."; return false; }
    g.dirty = false; g.status = L"Guardado en gowj.ini."; return true;
}
void BrowseIso()
{
    wchar_t file[MAX_PATH] = {}; std::wstring cur = Wide(IniValue("iso", "")); wcsncpy(file, cur.c_str(), MAX_PATH - 1);
    OPENFILENAMEW o{ sizeof(o) }; o.hwndOwner = g.wnd;
    o.lpstrFilter = L"ISO de Xbox 360 (*.iso)\0*.iso\0Todos los archivos\0*.*\0"; o.lpstrFile = file; o.nMaxFile = MAX_PATH;
    o.lpstrTitle = L"ISO de Gears of War: Judgment (Xbox 360)"; o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&o)) { SetIni("iso", Narrow(file)); g.status = L"ISO elegido."; }
}
void OpenFolder(const std::string& p) { std::filesystem::create_directories(Wide(p)); ShellExecuteW(nullptr, L"open", Wide(p).c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
void Click(int id)
{
    if (id >= kIdTextBase && id < kIdTextBase + kTextCount) { g.editing = id - kIdTextBase; return; }
    g.editing = -1;
    if (id >= kIdPageBase && id < kIdPageBase + kPages) { g.page = Page(id - kIdPageBase); return; }
    if (id >= kIdChoiceBase && id < kIdChoiceBase + kChoiceCount * 2)
    {
        int i = (id - kIdChoiceBase) / 2, dir = ((id - kIdChoiceBase) & 1) ? 1 : -1;
        const auto& o = kChoices[i].options; int k = ChoiceIndex(i);
        k = k < 0 ? 0 : (k + dir + int(o.size())) % int(o.size());
        SetIni(kChoices[i].key, o[size_t(k)].first);
        return;
    }
    if (id >= kIdToggleBase && id < kIdToggleBase + kToggleCount) { int i = id - kIdToggleBase; SetIni(kToggles[i].key, ToggleValue(i) ? "0" : "1"); return; }
    switch (id)
    {
    case kIdPlay:
        if (IniValue("iso", "").empty() || GetFileAttributesW(Wide(IniValue("iso", "")).c_str()) == INVALID_FILE_ATTRIBUTES)
        { BrowseIso(); if (IniValue("iso", "").empty()) { g.status = L"Elige el ISO del juego para jugar."; return; } }
        if (g.dirty) Save();
        g.play = true; g.done = true; break;
    case kIdSave: Save(); break;
    case kIdBrowseIso: BrowseIso(); break;
    case kIdOpenSaves: OpenFolder(DataDir()); break;
    case kIdOpenDlc: { std::string d = IniValue("dlc", "dlc"); OpenFolder((d.size() > 1 && d[1] == ':') ? d : ExeDirG() + "\\" + d); break; }
    case kIdOpenLogs: ShellExecuteW(nullptr, L"open", Wide(ExeDirG() + "\\gowj.log").c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
    }
}
int HitAt(int x, int y)
{
    for (auto it = g.hits.rbegin(); it != g.hits.rend(); ++it) if (x >= it->r.left && x < it->r.right && y >= it->r.top && y < it->r.bottom) return it->id;
    return 0;
}
LRESULT CALLBACK LauncherProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT rc; GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc); HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom); HGDIOBJ old = SelectObject(mem, bmp);
        Paint(mem, rc); BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem); EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE:
    {
        int hot = HitAt(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (hot != g.hover) { g.hover = hot; InvalidateRect(h, nullptr, FALSE); }
        SetCursor(LoadCursor(nullptr, hot ? IDC_HAND : IDC_ARROW));
        TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 }; TrackMouseEvent(&t);
        return 0;
    }
    case WM_MOUSELEAVE: g.hover = 0; InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_SETCURSOR: if (LOWORD(l) == HTCLIENT) return TRUE; break;
    case WM_LBUTTONDOWN:
    {
        int id = HitAt(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (id) { g.status.clear(); Click(id); } else g.editing = -1;
        InvalidateRect(h, nullptr, FALSE);
        if (g.done) DestroyWindow(h);
        return 0;
    }
    case WM_CHAR:
        if (g.editing >= 0)
        {
            const char* key = kTexts[g.editing].key; std::string v = IniValue(key, kTexts[g.editing].fallback);
            if (w == 8) { if (!v.empty()) v.pop_back(); }
            else if (w == 13 || w == 27) g.editing = -1;
            else if (w >= 32 && w < 127 && v.size() < 64) v += char(w);
            SetIni(key, v); InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (g.editing >= 0) { if (w == VK_RETURN || w == VK_ESCAPE) { g.editing = -1; InvalidateRect(h, nullptr, FALSE); } return 0; }
        if (w == VK_RETURN) { Click(kIdPlay); if (g.done) DestroyWindow(h); else InvalidateRect(h, nullptr, FALSE); return 0; }
        break;
    case WM_TIMER:
        if (g.editing >= 0) { static ULONGLONG last = 0; if (GetTickCount64() - last >= 500) { last = GetTickCount64(); InvalidateRect(h, nullptr, FALSE); } }
        return 0;
    case WM_CLOSE:
        if (g.dirty && MessageBoxW(h, L"Hay cambios sin guardar. ¿Guardarlos antes de salir?", L"Gears of War: Judgment", MB_YESNO | MB_ICONQUESTION) == IDYES) Save();
        g.done = true; DestroyWindow(h);
        return 0;
    case WM_DPICHANGED:
    {
        g.dpi = HIWORD(w); RECT* r = reinterpret_cast<RECT*>(l);
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}
void MakeFonts()
{
    const wchar_t* face = L"Bahnschrift";
    g.title = MakeFont(46, FW_BOLD, face); g.subtitle = MakeFont(24, FW_BOLD, face); g.heading = MakeFont(15, FW_SEMIBOLD, face);
    g.body = MakeFont(14, FW_NORMAL, face); g.smallFont = MakeFont(12, FW_NORMAL, face); g.button = MakeFont(14, FW_SEMIBOLD, face); g.playFont = MakeFont(20, FW_BOLD, face);
}
} // namespace

bool LauncherWanted(int argc, char** argv)
{
    if (getenv("GOWJ_NOLAUNCHER")) return false;
    for (int i = 1; i < argc; i++) { if (!_stricmp(argv[i], "--jugar")) return false; if (!_stricmp(argv[i], "--launcher")) return true; }
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) return true;
    IniFile ini; ini.Load(ExeDirG() + "\\gowj.ini");
    return ini.Get("mostrar_launcher", "1") != "0";
}
// Muestra el launcher. true = JUGAR (gowj.ini ya guardado), false = salir
bool RunLauncherUI()
{
    DPI_AWARENESS_CONTEXT oldDpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g.ini.Load(ExeDirG() + "\\gowj.ini");
    if (g.ini.lines.empty()) g.ini.lines = { "# Gears of War: Judgment (port PC). Lo escribe el launcher; las claves que no conoce se conservan." };
    WNDCLASSW wc{}; wc.lpfnWndProc = LauncherProc; wc.hInstance = GetModuleHandleW(nullptr); wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1)); wc.lpszClassName = L"GOWJLauncher";
    RegisterClassW(&wc);
    HMONITOR mon = MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    UINT dx = 96, dy = 96;
    using GetDpi = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    if (auto f = (GetDpi)GetProcAddress(LoadLibraryW(L"shcore.dll"), "GetDpiForMonitor")) f(mon, 0, &dx, &dy);
    g.dpi = int(dx);
    MakeFonts(); LoadPictures();
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r{ 0, 0, S(1120), S(760) };
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, UINT(g.dpi));
    MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(mon, &mi);
    int w = r.right - r.left, h = r.bottom - r.top;
    int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2, y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2;
    g.wnd = CreateWindowW(wc.lpszClassName, L"Gears of War: Judgment · Port PC", style, x, y, w, h, nullptr, nullptr, wc.hInstance, nullptr);
    BOOL dark = TRUE; DwmSetWindowAttribute(g.wnd, 20, &dark, sizeof(dark));
    COLORREF caption = RGB(8, 8, 9), border = kRedDim;
    DwmSetWindowAttribute(g.wnd, 35, &caption, sizeof(caption)); DwmSetWindowAttribute(g.wnd, 34, &border, sizeof(border));
    if (const char* page = getenv("GOWJ_LAUNCHER_PAGE")) g.page = Page(std::clamp(atoi(page), 0, int(kPages) - 1));   // capturas de cada pagina
    ShowWindow(g.wnd, SW_SHOW);
    SetTimer(g.wnd, 1, 30, nullptr);
    MSG msg;
    while (!g.done && GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (IsWindow(g.wnd)) DestroyWindow(g.wnd);
    for (HFONT f : { g.title, g.subtitle, g.heading, g.body, g.smallFont, g.button, g.playFont }) DeleteObject(f);
    FreePictures();
    SetThreadDpiAwarenessContext(oldDpi);
    return g.play;
}
