// Sistema de archivos (sesion 4).
//  - Disco del juego (game:, d:, \Device\Cdrom0): se lee DIRECTAMENTE del ISO (XDVDFS, particion detectada: XGD3 0x2080000 / XGD2 0xFD90000).
//  - Resto de dispositivos (cache:, hdd:, \Device\Harddisk0\...): carpeta del PC  <dir_datos>\<dispositivo>\...
#include "objects.h"
#include <ppc_recomp_shared.h>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <string>
#include <algorithm>
#include <cstring>
#include <memory>
void TexDirtyTouch(uint32_t addr, uint32_t len);   // texdirty.cpp (sesion 10)
void InputScriptNotifyOpen(const std::string& path);   // xam.cpp (anclas del guion de entrada)

namespace
{
uint64_t kIsoPartition = 0x2080000;   // GoWJ: XGD3 (0x2080000); se detecta en InitFileSystem (XGD2 = 0xFD90000)
constexpr uint32_t kSector = 2048;

struct IsoEntry { uint64_t offset; uint32_t size; bool dir; std::string name; std::vector<std::string> children; std::shared_ptr<std::vector<uint8_t>> mem; };   // mem: contenido sustituido (ini, sesion 10)
std::unordered_map<std::string, IsoEntry> g_iso;   // ruta en minusculas sin barra inicial ("" = raiz)
HANDLE g_isoFile = INVALID_HANDLE_VALUE;
std::mutex g_isoMutex;
std::string g_dataDir;                              // carpeta para dispositivos escribibles
std::mutex g_linkMutex;
std::vector<std::pair<std::string, std::string>> g_links; // enlace simbolico (minusculas) -> destino

std::string Lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }

bool IsoRead(uint64_t off, void* dst, uint32_t len)
{
    std::lock_guard lock(g_isoMutex);
    OVERLAPPED ov{}; ov.Offset = (DWORD)off; ov.OffsetHigh = (DWORD)(off >> 32);
    DWORD got = 0;
    return ReadFile(g_isoFile, dst, len, &got, &ov) && got == len;
}

void IsoWalk(uint32_t sector, uint32_t size, const std::string& prefix)
{
    std::vector<uint8_t> d(size);
    if (!size || !IsoRead(kIsoPartition + uint64_t(sector) * kSector, d.data(), size)) return;
    // GoWJ sesion 10: clave en minusculas como las entradas (antes "GearGame" != "geargame": los subdirectorios del ISO no se
    // podian listar y el juego no encontraba Xbox360TOC_ESM.txt -> sin idiomas, siempre INT)
    auto& parent = g_iso[prefix.empty() ? "" : Lower(prefix.substr(0, prefix.size() - 1))];
    std::vector<uint32_t> stack{ 0 };
    while (!stack.empty())
    {
        uint32_t off = stack.back() * 4; stack.pop_back();
        if (off + 14 > size) continue;
        uint16_t left = d[off] | (d[off + 1] << 8), right = d[off + 2] | (d[off + 3] << 8);
        if (left == 0xFFFF) continue;
        uint32_t sec, sz; memcpy(&sec, &d[off + 4], 4); memcpy(&sz, &d[off + 8], 4);
        uint8_t attr = d[off + 12], nl = d[off + 13];
        std::string name((const char*)&d[off + 14], nl);
        std::string path = Lower(prefix + name);
        IsoEntry e{ kIsoPartition + uint64_t(sec) * kSector, sz, (attr & 0x10) != 0, name, {} };
        g_iso[path] = e;
        parent.children.push_back(path);
        if (e.dir) IsoWalk(sec, sz, prefix + name + "\\");
        if (left) stack.push_back(left);
        if (right) stack.push_back(right);
    }
}

// Ruta de invitado -> (es_iso, ruta relativa en minusculas)
bool ResolvePath(std::string p, std::string& rel, std::string& device)
{
    std::replace(p.begin(), p.end(), '/', '\\');
    std::string lp = Lower(p);
    {
        std::lock_guard lock(g_linkMutex);
        for (auto& [link, target] : g_links)
            if (lp.compare(0, link.size(), link) == 0) { lp = Lower(target) + "\\" + lp.substr(link.size()); break; }
    }
    if (lp.compare(0, 4, "\\??\\") == 0) lp = lp.substr(4);
    static const char* cdrom[] = { "\\device\\cdrom0", "game:", "d:", "\\systemroot" };
    for (auto* c : cdrom)
        if (lp.compare(0, strlen(c), c) == 0) { rel = lp.substr(strlen(c)); device = ""; goto iso; }
    {
        size_t colon = lp.find(':');
        if (colon != std::string::npos) { device = lp.substr(0, colon); rel = lp.substr(colon + 1); }
        else if (lp.compare(0, 18, "\\device\\harddisk0\\") == 0) { device = "hdd"; rel = lp.substr(18); size_t s = rel.find('\\'); rel = s == std::string::npos ? "" : rel.substr(s); }
        else { device = ""; rel = lp; goto iso; }
        while (!rel.empty() && rel[0] == '\\') rel.erase(0, 1);
        return false;
    }
iso:
    while (!rel.empty() && rel[0] == '\\') rel.erase(0, 1);
    while (!rel.empty() && rel.back() == '\\') rel.pop_back();
    // normalizar "\\" dobles y "."
    std::string out; out.reserve(rel.size());
    for (size_t i = 0; i < rel.size(); i++) { if (rel[i] == '\\' && !out.empty() && out.back() == '\\') continue; out += rel[i]; }
    rel = out;
    return true;
}

std::string HostPath(const std::string& device, const std::string& rel)
{
    return g_dataDir + "\\" + (device.empty() ? "misc" : device) + (rel.empty() ? "" : "\\" + rel);
}

struct FileObj : KObject
{
    bool iso = true;
    std::string path;            // ruta relativa (iso) o ruta del PC
    const IsoEntry* entry = nullptr;
    HANDLE host = INVALID_HANDLE_VALUE;
    uint64_t position = 0;
    bool dir = false;
    size_t enumIndex = 0;
    std::string enumPattern;   // sesion 10: el patron de la primera llamada vale para toda la enumeracion (como NT)
    std::vector<std::string> hostList; // enumeracion de directorios del PC
    bool rawDevice = false;      // NG2: particion en bruto (\Device\Harddisk0\partitionN): lecturas tras el final = ceros
    std::mutex io;               // GoWJ sesion 10: posicion + lectura atomicas (varios hilos leen el mismo paquete)
    ~FileObj() override { if (host != INVALID_HANDLE_VALUE) CloseHandle(host); }
};

std::string ReadAnsiString(uint32_t ansiPtr)
{
    if (!ansiPtr) return {};
    uint16_t len = __builtin_bswap16(*(uint16_t*)(g_base + ansiPtr));
    uint32_t buf = Load32(ansiPtr + 4);
    return buf ? std::string((const char*)(g_base + buf), len) : std::string();
}
// X_OBJECT_ATTRIBUTES {root_directory, name_ptr (ANSI_STRING*), attributes}
std::string ObjectPath(uint32_t attrPtr)
{
    uint32_t root = Load32(attrPtr);
    std::string name = ReadAnsiString(Load32(attrPtr + 4));
    if (root && root != 0xFFFFFFFD)
        if (auto o = std::dynamic_pointer_cast<FileObj>(GetHandleObject(root)))
            return (o->iso ? "game:\\" : "") + o->path + "\\" + name;
    return name;
}

void SetIoStatus(uint32_t io, uint32_t status, uint32_t info) { if (io) { Store32(io, status); Store32(io + 4, info); } }

bool GlobMatch(const char* p, const char* s)
{
    if (!*p) return !*s;
    if (*p == '*') return GlobMatch(p + 1, s) || (*s && GlobMatch(p, s + 1));
    if (*s && (*p == '?' || tolower((unsigned char)*p) == tolower((unsigned char)*s))) return GlobMatch(p + 1, s + 1);
    return false;
}

void WriteNetworkOpenInfo(uint32_t p, uint64_t size, bool dir)
{
    memset(g_base + p, 0, 0x38);
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    uint64_t t = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    for (int i = 0; i < 4; i++) Store64(p + i * 8, t);
    Store64(p + 0x20, (size + 0xFFF) & ~0xFFFull);
    Store64(p + 0x28, size);
    Store32(p + 0x30, dir ? 0x10 : 0x80); // FILE_ATTRIBUTE_DIRECTORY / NORMAL
}

uint32_t OpenCommonImpl(uint32_t handlePtr, const std::string& guestPath, uint32_t ioStatus, uint32_t disposition, uint32_t options);
bool g_logFiles = getenv("GOWJ_NOLOGFILES") == nullptr;
uint32_t OpenCommon(uint32_t handlePtr, uint32_t attrPtr, uint32_t ioStatus, uint32_t disposition, uint32_t options)
{
    std::string guestPath = ObjectPath(attrPtr), rel, device;
    // GOWJ_SKIPVIDEO=1 (diagnostico, sesion 4): los .wmv "no existen" -> el juego se salta los videos
    static const bool skipVideo = getenv("GOWJ_SKIPVIDEO") && atoi(getenv("GOWJ_SKIPVIDEO")) != 0;
    if (skipVideo && guestPath.size() > 4 && _stricmp(guestPath.c_str() + guestPath.size() - 4, ".wmv") == 0)
    {
        if (g_logFiles) LOG("[fs] abrir \"%s\" -> omitido (GOWJ_SKIPVIDEO)", guestPath.c_str());
        SetIoStatus(ioStatus, XSTATUS_OBJECT_NAME_NOT_FOUND, 0);
        return XSTATUS_OBJECT_NAME_NOT_FOUND;
    }
    uint32_t st = OpenCommonImpl(handlePtr, guestPath, ioStatus, disposition, options);
    if (g_logFiles) LOG("[fs] abrir \"%s\" -> 0x%X", guestPath.c_str(), st);
    ::InputScriptNotifyOpen(guestPath);
    return st;
}

uint32_t OpenCommonImpl(uint32_t handlePtr, const std::string& guestPath, uint32_t ioStatus, uint32_t disposition, uint32_t options)
{
    std::string rel, device;
    auto f = std::make_shared<FileObj>();
    if (ResolvePath(guestPath, rel, device))
    {
        auto it = g_iso.find(rel);
        if (it == g_iso.end()) { SetIoStatus(ioStatus, XSTATUS_OBJECT_NAME_NOT_FOUND, 0); return XSTATUS_OBJECT_NAME_NOT_FOUND; }
        if (disposition == 2 /*FILE_CREATE*/) { SetIoStatus(ioStatus, XSTATUS_ACCESS_DENIED, 0); return XSTATUS_ACCESS_DENIED; }
        f->iso = true; f->path = rel; f->entry = &it->second; f->dir = it->second.dir;
    }
    else
    {
        std::string hp = HostPath(device, rel);
        f->iso = false; f->path = hp;
        DWORD attrs = GetFileAttributesA(hp.c_str());
        bool wantDir = (options & 1) != 0; // FILE_DIRECTORY_FILE
        // NG2 (sesion 5): \Device\Harddisk0\partitionN abierta como archivo (sin FILE_DIRECTORY_FILE) = disco en bruto.
        // El juego lee/firma/escribe ahi la cabecera de su cache. Se guarda en <datos>\Harddisk0_partitionN.img.
        if (_strnicmp(guestPath.c_str(), "\\Device\\Harddisk0\\partition", 27) == 0) LOG("[fs] particion: rel='%s' disp=%s opciones=%08X", rel.c_str(), device.c_str(), options);
        if (rel.empty() && !(options & 1) && (_strnicmp(guestPath.c_str(), "\\Device\\Harddisk0\\partition", 27) == 0 ||
            (_strnicmp(guestPath.c_str(), "\\Device\\Harddisk0\\Cache", 23) == 0 && guestPath.back() != '\\')))
        {
            CreateDirectoryA(g_dataDir.c_str(), nullptr);
            std::string img = g_dataDir + "\\Harddisk0_" + guestPath.substr(18) + ".img";
            f->host = CreateFileA(img.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f->host == INVALID_HANDLE_VALUE) { SetIoStatus(ioStatus, XSTATUS_ACCESS_DENIED, 0); return XSTATUS_ACCESS_DENIED; }
            f->path = img; f->rawDevice = true; f->type = 0xFE;
            Store32(handlePtr, InsertHandle(f));
            SetIoStatus(ioStatus, XSTATUS_SUCCESS, 1);
            return XSTATUS_SUCCESS;
        }
        // Raiz del dispositivo (p. ej. \Device\Harddisk0\partition0): existe siempre, se trata como directorio
        if (rel.empty()) { CreateDirectoryA(g_dataDir.c_str(), nullptr); CreateDirectoryA(hp.c_str(), nullptr); wantDir = true; attrs = GetFileAttributesA(hp.c_str()); }
        if (wantDir || (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)))
        {
            if (attrs == INVALID_FILE_ATTRIBUTES)
            {
                if (disposition == 1 /*FILE_OPEN*/) { SetIoStatus(ioStatus, XSTATUS_OBJECT_NAME_NOT_FOUND, 0); return XSTATUS_OBJECT_NAME_NOT_FOUND; }
                std::string acc; // crear la ruta completa
                for (size_t i = 0; i <= hp.size(); i++) if (i == hp.size() || hp[i] == '\\') { acc = hp.substr(0, i); CreateDirectoryA(acc.c_str(), nullptr); }
            }
            f->dir = true;
        }
        else
        {
            // FILE_SUPERSEDE 0, FILE_OPEN 1, FILE_CREATE 2, FILE_OPEN_IF 3, FILE_OVERWRITE 4, FILE_OVERWRITE_IF 5
            static const DWORD map[] = { CREATE_ALWAYS, OPEN_EXISTING, CREATE_NEW, OPEN_ALWAYS, TRUNCATE_EXISTING, CREATE_ALWAYS };
            if (disposition != 1)
            {
                std::string acc; size_t last = hp.find_last_of('\\');
                for (size_t i = 0; i <= last && last != std::string::npos; i++) if (i == last || hp[i] == '\\') { acc = hp.substr(0, i); CreateDirectoryA(acc.c_str(), nullptr); }
            }
            f->host = CreateFileA(hp.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  map[disposition < 6 ? disposition : 1], FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f->host == INVALID_HANDLE_VALUE)
            {
                uint32_t st = GetLastError() == ERROR_FILE_EXISTS ? XSTATUS_OBJECT_NAME_COLLISION : XSTATUS_OBJECT_NAME_NOT_FOUND;
                SetIoStatus(ioStatus, st, 0); return st;
            }
        }
    }
    f->type = 0xFE;
    Store32(handlePtr, InsertHandle(f));
    SetIoStatus(ioStatus, XSTATUS_SUCCESS, 1 /*FILE_OPENED*/);
    return XSTATUS_SUCCESS;
}

std::shared_ptr<FileObj> GetFile(uint32_t h) { return std::dynamic_pointer_cast<FileObj>(GetHandleObject(h)); }

uint32_t ReadInto(FileObj& f, uint32_t dst, uint32_t len, uint32_t offsetPtr, uint32_t& got)
{
    // GoWJ sesion 10: el hilo de carga asincrona y otros leian el mismo paquete a la vez con desplazamiento explicito; uno pisaba
    // la posicion del otro y copiaba datos de otro desplazamiento (fuente del HUD con trozos de otras filas, iconos rotos; de forma
    // intermitente). Posicion y lectura bajo el cerrojo del archivo. GOWJ_NOFILELOCK=1 = antes.
    static const bool noLock = getenv("GOWJ_NOFILELOCK") != nullptr;
    std::unique_lock<std::mutex> lk(f.io, std::defer_lock); if (!noLock) lk.lock();
    if (offsetPtr)
    {
        uint64_t o = __builtin_bswap64(*(uint64_t*)(g_base + offsetPtr));
        if (o != 0xFFFFFFFFFFFFFFFEull) f.position = o;
    }
    got = 0;
    if (f.iso)
    {
        if (f.position >= f.entry->size) return XSTATUS_END_OF_FILE;
        uint32_t n = (uint32_t)std::min<uint64_t>(len, f.entry->size - f.position);
        if (f.entry->mem) memcpy(g_base + dst, f.entry->mem->data() + f.position, n);   // ini reescrito en memoria (sesion 10)
        else { TexDirtyTouch(dst, n); if (!IsoRead(f.entry->offset + f.position, g_base + dst, n)) return 0xC0000185; } // XSTATUS_IO_DEVICE_ERROR (sesion 10: paginas vigiladas)
        got = n;
        // Sesion 24: saltar_intro=1 (GOWJ_SKIPINTRO): el video de logos (Bink con palabras de 32 bits invertidas) se anuncia con
        // 1 solo cuadro (palabras 2 y 4 de la cabecera = numero de cuadros); el juego lo da por terminado y sigue
        static const bool skipIntro = getenv("GOWJ_SKIPINTRO") && atoi(getenv("GOWJ_SKIPINTRO")) != 0;
        // GoWJ sesion 9: en Gears el video de logos es Movies\Open_Logos.bik (Bink normal: cuadros en little-endian en 8 y 16)
        if (skipIntro && f.position <= 8 && f.position + n >= 20 && f.path.size() >= 14 &&
            _stricmp(f.path.c_str() + f.path.size() - 14, "open_logos.bik") == 0)
        {
            uint8_t* h = g_base + dst - f.position;
            if (memcmp(h, "BIK", 3) == 0 || memcmp(h, "KB2", 3) == 0)
            {
                static const uint8_t one[4] = { 1, 0, 0, 0 };
                memcpy(h + 8, one, 4); memcpy(h + 16, one, 4);
                LOG("[fs] saltar intro: video de logos reducido a 1 cuadro");
            }
        }
        if (skipIntro && f.position <= 8 && f.position + n >= 20 && f.path.size() >= 18 &&
            _stricmp(f.path.c_str() + f.path.size() - 18, "logo_intro.bik.xen") == 0)
        {
            uint8_t* h = g_base + dst - f.position;
            if (memcmp(h, "iKIB", 4) == 0)
            {
                static const uint8_t one[4] = { 0, 0, 0, 1 };
                memcpy(h + 8, one, 4); memcpy(h + 16, one, 4);
                LOG("[fs] saltar intro: video de logos reducido a 1 cuadro");
            }
        }
    }
    else
    {
        LARGE_INTEGER li; li.QuadPart = (LONGLONG)f.position;
        SetFilePointerEx(f.host, li, nullptr, FILE_BEGIN);
        DWORD n = 0;
        TexDirtyTouch(dst, len);   // sesion 10: el kernel no puede escribir en paginas de textura vigiladas (texdirty.cpp)
        ReadFile(f.host, g_base + dst, len, &n, nullptr);
        if (f.rawDevice && n < len) { memset(g_base + dst + n, 0, len - n); n = len; }   // disco en bruto: ceros
        if (n == 0 && len) return XSTATUS_END_OF_FILE;
        got = n;
    }
    f.position += got;
    return XSTATUS_SUCCESS;
}

void SignalEvent(uint32_t eventHandle) { if (eventHandle) if (auto e = GetHandleObject(eventHandle)) SetEvent(e->waitable); }
} // namespace

// Se llama desde main antes de _xstart
// ---- GoWJ sesion 10 (del port de Gears 3, sesion 3/5): ajustes de los ini del juego reescribiendo Coalesced_*.bin en memoria ----
// Formato: int32 BE n archivos; archivo: nombre, n secciones; seccion: nombre, n claves; clave: nombre, valor. Cadenas: int32 BE
// (negativo = UTF-16LE de -n caracteres con el nulo; positivo = ANSI de n bytes con el nulo). Ajustes "Seccion:Clave=Valor" de
// GOWJ_INI (separados por ';') y los del port: sin aceleracion de vista (con el raton los giros rapidos se disparaban; GOWJ_VIEWACCEL=1
// la deja).
namespace {
struct CoStr { std::string s; bool wide; };
bool CoRead(const std::vector<uint8_t>& d, size_t& o, CoStr& out)
{
    if (o + 4 > d.size()) return false;
    int32_t n = (int32_t)((d[o] << 24) | (d[o + 1] << 16) | (d[o + 2] << 8) | d[o + 3]); o += 4;
    out.s.clear(); out.wide = n < 0;
    if (n < 0) { size_t c = (size_t)-n; if (o + 2 * c > d.size()) return false; for (size_t i = 0; i < c; i++) { uint16_t ch = d[o + 2 * i] | (d[o + 2 * i + 1] << 8); if (ch) out.s += ch < 128 ? (char)ch : '?'; } o += 2 * c; }
    else { if (o + n > d.size()) return false; for (int32_t i = 0; i < n; i++) if (d[o + i]) out.s += (char)d[o + i]; o += n; }
    return true;
}
void CoWriteRaw(std::vector<uint8_t>& w, const std::vector<uint8_t>& d, size_t from, size_t to) { w.insert(w.end(), d.begin() + from, d.begin() + to); }
void CoWrite(std::vector<uint8_t>& w, const std::string& s, bool wide)
{
    int32_t n = (int32_t)s.size() + 1; if (wide) n = -n;
    for (int k = 3; k >= 0; k--) w.push_back((uint8_t)(n >> (k * 8)));
    for (char c : s) { w.push_back((uint8_t)c); if (wide) w.push_back(0); }
    w.push_back(0); if (wide) w.push_back(0);
}
int32_t CoInt(const std::vector<uint8_t>& d, size_t& o) { int32_t v = (int32_t)((d[o] << 24) | (d[o + 1] << 16) | (d[o + 2] << 8) | d[o + 3]); o += 4; return v; }
}
void ApplyIniOverrides()
{
    std::vector<std::pair<std::string, std::string>> ov;
    if (!getenv("GOWJ_VIEWACCEL")) ov.push_back({ "GearGame.GearPlayerInput:bViewAccelEnabled", "FALSE" });
    // GoWJ sesion 11: desenfoque de movimiento desactivado por defecto. En el port emborrona a los personajes que van con la camara
    // (en la consola salen nitidos: falla el bufer de velocidades de los personajes) y se ve como si vibraran. GOWJ_MOTIONBLUR=1 (o
    // desenfoque_movimiento=1 en gowj.ini) lo deja como en la consola.
    if (!getenv("GOWJ_MOTIONBLUR"))
    {
        ov.push_back({ "SystemSettings:MotionBlur", "False" });
        ov.push_back({ "SystemSettings:MotionBlurSkinning", "0" });
        // Tambien el desenfoque radial (al correr agachado y con explosiones): el usuario seguia viendo la imagen emborronada al moverse
        ov.push_back({ "SystemSettings:AllowRadialBlur", "False" });
    }
    if (const char* e = getenv("GOWJ_INI"))
    {
        std::string s = e; size_t p = 0;
        while (p < s.size()) { size_t q = s.find(';', p); if (q == std::string::npos) q = s.size(); std::string it = s.substr(p, q - p); size_t eq = it.find('='); if (eq != std::string::npos) ov.push_back({ it.substr(0, eq), it.substr(eq + 1) }); p = q + 1; }
    }
    static const char* grep = getenv("GOWJ_INIGREP");   // diagnostico (GoWJ sesion 11): registra las claves que contienen ese texto
    if (ov.empty() && !grep) return;
    for (auto& [path, e] : g_iso)
    {
        if (e.dir || path.find("coalesced_") == std::string::npos || path.size() < 4 || path.substr(path.size() - 4) != ".bin") continue;
        std::vector<uint8_t> d(e.size);
        if (!IsoRead(e.offset, d.data(), e.size)) continue;
        std::vector<uint8_t> w; w.reserve(d.size() + 4096);
        size_t o = 0; int changed = 0; bool ok = d.size() >= 4;
        int32_t nf = ok ? CoInt(d, o) : 0; if (ok) CoWriteRaw(w, d, 0, 4);
        for (int32_t fi = 0; fi < nf && ok; fi++)
        {
            size_t a = o; CoStr fn; ok &= CoRead(d, o, fn); if (!ok || o + 4 > d.size()) { ok = false; break; } int32_t ns = CoInt(d, o); CoWriteRaw(w, d, a, o);
            for (int32_t si = 0; si < ns && ok; si++)
            {
                size_t b = o; CoStr sn; ok &= CoRead(d, o, sn); if (!ok || o + 4 > d.size()) { ok = false; break; } int32_t nk = CoInt(d, o); CoWriteRaw(w, d, b, o);
                for (int32_t ki = 0; ki < nk && ok; ki++)
                {
                    size_t c = o; CoStr k, v; ok &= CoRead(d, o, k); size_t vo = o; ok &= CoRead(d, o, v);
                    if (!ok) break;
                    if (grep && path.find("_esm") != std::string::npos)
                    {   std::string kl = sn.s + ":" + k.s; std::string a = kl, g = grep; for (auto& ch : a) ch = (char)tolower(ch); for (auto& ch : g) ch = (char)tolower(ch);
                        if (a.find(g) != std::string::npos) LOG("[inigrep] %s [%s] %s=%s", fn.s.c_str(), sn.s.c_str(), k.s.c_str(), v.s.c_str()); }
                    const std::string* repl = nullptr;
                    for (auto& [sk, val] : ov) if (_stricmp(sk.c_str(), (sn.s + ":" + k.s).c_str()) == 0) repl = &val;
                    if (repl) { CoWriteRaw(w, d, c, vo); CoWrite(w, *repl, v.wide); changed++;
                        LOG("[ini] %s %s [%s] %s: %s -> %s", path.c_str(), fn.s.c_str(), sn.s.c_str(), k.s.c_str(), v.s.c_str(), repl->c_str()); }
                    else CoWriteRaw(w, d, c, o);
                }
            }
        }
        if (!ok || o != d.size()) { LOG("[ini] %s: formato no reconocido (leido %zu de %zu), sin cambios", path.c_str(), o, d.size()); continue; }
        if (changed) { e.mem = std::make_shared<std::vector<uint8_t>>(std::move(w)); e.size = (uint32_t)e.mem->size(); }
    }
}

bool InitFileSystem(const char* isoPath, const char* dataDir)
{
    g_dataDir = dataDir;
    CreateDirectoryA(dataDir, nullptr);
    g_isoFile = CreateFileA(isoPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (g_isoFile == INVALID_HANDLE_VALUE) { LOG("[fs] no se pudo abrir el ISO: %s", isoPath); return false; }
    char magic[20];
    uint32_t root[2];
    bool found = false;
    for (uint64_t cand : { 0x2080000ull, 0xFD90000ull, 0x18300000ull, 0ull })
        if (IsoRead(cand + 32 * kSector, magic, 20) && memcmp(magic, "MICROSOFT*XBOX*MEDIA", 20) == 0) { kIsoPartition = cand; found = true; break; }
    if (!found) { LOG("[fs] ISO sin XDVDFS"); return false; }
    LOG("[fs] XDVDFS en 0x%llX", (unsigned long long)kIsoPartition);
    IsoRead(kIsoPartition + 32 * kSector + 20, root, 8);
    g_iso[""] = IsoEntry{ 0, 0, true, "", {} };
    IsoWalk(root[0], root[1], "");
    LOG("[fs] ISO indexado: %zu entradas", g_iso.size() - 1);
    ApplyIniOverrides();
    return true;
}

// Lee un archivo completo del ISO (sesion 23, para el lanzador: default.xex). Ruta relativa a la raiz, p. ej. "default.xex".
std::vector<uint8_t> FsReadIsoFile(const char* path)
{
    std::vector<uint8_t> out;
    auto it = g_iso.find(Lower(path));
    if (it == g_iso.end() || it->second.dir) return out;
    out.resize(it->second.size);
    if (it->second.mem) out = *it->second.mem;
    else if (!IsoRead(it->second.offset, out.data(), it->second.size)) out.clear();
    return out;
}

// NtCreateFile(handle_ptr, desired_access, obj_attributes, io_status, allocation_size, file_attributes, share_access, creation_disposition)
// La 9a (create_options) va en la pila: r1+0x54
GUEST_IMPL(NtCreateFile)
{
    uint32_t options = Load32(ctx.r1.u32 + 0x54);
    ctx.r3.u64 = OpenCommon(ctx.r3.u32, ctx.r5.u32, ctx.r6.u32, ctx.r10.u32, options);
}
// NtOpenFile(handle_ptr, desired_access, obj_attributes, io_status, share_access, open_options)
GUEST_IMPL(NtOpenFile) { ctx.r3.u64 = OpenCommon(ctx.r3.u32, ctx.r5.u32, ctx.r6.u32, 1, ctx.r8.u32); }

// NG2 (sesion 4): rutina APC de E/S (apc_routine(apc_context, io_status_block, 0)). Se llama en el acto, al terminar la
// operacion (en la consola se entregaria en la siguiente espera "alertable" del hilo; el juego solo necesita que llegue).
static void CallIoApc(PPCContext& ctx, uint8_t* base, uint32_t routine, uint32_t apcContext, uint32_t ioStatus)
{
    routine &= ~3u;   // el bit bajo es un indicador de XAPI (p. ej. 0x838199D9 -> BasepIoCompletion 0x838199D8)
    PPCFunc* fn = (routine >= PPC_CODE_BASE && routine < PPC_CODE_BASE + PPC_CODE_SIZE) ? PPC_LOOKUP_FUNC(base, routine) : nullptr;
    static int nlog = 0;
    if (nlog++ < 5) LOG("[fs] APC de E/S: rutina %08X contexto %08X io %08X", routine, apcContext, ioStatus);
    if (!fn) { LOG("[fs] rutina APC %08X sin funcion traducida", routine); return; }
    PPCContext saved = ctx;
    ctx.r3.u64 = apcContext; ctx.r4.u64 = ioStatus; ctx.r5.u64 = 0;
    fn(ctx, base);
    uint64_t r1 = saved.r1.u64; ctx = saved; ctx.r1.u64 = r1;
}
// Cola de APC de E/S por hilo: se entregan en la siguiente espera "alertable" del hilo (como en la consola).
// Llamarlas en el acto dentro de NtReadFile rompia la carga de fondo del juego (la rutina marcaba "terminado" antes de
// que el juego registrase la peticion como pendiente).
#include <deque>
namespace { struct PendingApc { uint32_t routine, apcContext, ioStatus; }; thread_local std::deque<PendingApc> t_apcs; }
static void QueueIoApc(uint32_t routine, uint32_t apcContext, uint32_t ioStatus) { t_apcs.push_back({ routine, apcContext, ioStatus }); }
bool DeliverIoApcs(PPCContext& ctx, uint8_t* base)
{
    if (t_apcs.empty()) return false;
    while (!t_apcs.empty()) { PendingApc a = t_apcs.front(); t_apcs.pop_front(); CallIoApc(ctx, base, a.routine, a.apcContext, a.ioStatus); }
    return true;
}
// NtReadFile(handle, event, apc_routine, apc_context, io_status, buffer, length, byte_offset_ptr)
GUEST_IMPL(NtReadFile)
{
    auto f = GetFile(ctx.r3.u32);
    if (!f || f->dir) { ctx.r3.u64 = XSTATUS_INVALID_HANDLE; return; }
    uint64_t pos0 = ctx.r10.u32 ? __builtin_bswap64(*(uint64_t*)(g_base + ctx.r10.u32)) : f->position;
    uint32_t got = 0, st = ReadInto(*f, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32, got);
    SetIoStatus(ctx.r7.u32, st, got);
    SignalEvent(ctx.r4.u32);
    static const bool readLog = getenv("GOWJ_READLOG") != nullptr;   // NG2 (sesion 3): cada lectura
    if (readLog) LOG("[fs] leer %s @0x%llX 0x%X -> 0x%X en %08X (st 0x%X, evento %08X, apc %08X)", f->path.c_str(), (unsigned long long)pos0, ctx.r9.u32, got, ctx.r8.u32, st, ctx.r4.u32, ctx.r5.u32);
    if (ctx.r5.u32) QueueIoApc(ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
    ctx.r3.u64 = st;
}
// NtReadFileScatter(handle, event, apc_routine, apc_context, io_status, segment_array, length, byte_offset_ptr)
GUEST_IMPL(NtReadFileScatter)
{
    auto f = GetFile(ctx.r3.u32);
    if (!f) { ctx.r3.u64 = XSTATUS_INVALID_HANDLE; return; }
    uint32_t total = 0, st = XSTATUS_SUCCESS, left = ctx.r9.u32, offPtr = ctx.r10.u32;
    for (uint32_t i = 0; left && st == XSTATUS_SUCCESS; i++)
    {
        uint32_t page = Load32(ctx.r8.u32 + i * 8); // FILE_SEGMENT_ELEMENT (64 bits, direccion en la mitad baja)
        if (!page) page = Load32(ctx.r8.u32 + i * 8 + 4);
        uint32_t chunk = std::min<uint32_t>(left, 0x1000), got = 0;
        st = ReadInto(*f, page, chunk, offPtr, got);
        offPtr = 0; total += got; left -= chunk;
        if (got < chunk) break;
    }
    if (total && st == XSTATUS_END_OF_FILE) st = XSTATUS_SUCCESS;
    SetIoStatus(ctx.r7.u32, st, total);
    SignalEvent(ctx.r4.u32);
    if (ctx.r5.u32) QueueIoApc(ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
    ctx.r3.u64 = st;
}
// NtWriteFile(handle, event, apc_routine, apc_context, io_status, buffer, length, byte_offset_ptr)
GUEST_IMPL(NtWriteFile)
{
    auto f = GetFile(ctx.r3.u32);
    if (!f || f->iso || f->dir) { SetIoStatus(ctx.r7.u32, XSTATUS_ACCESS_DENIED, 0); ctx.r3.u64 = f ? XSTATUS_ACCESS_DENIED : XSTATUS_INVALID_HANDLE; return; }
    std::lock_guard<std::mutex> wlk(f->io);   // sesion 10: posicion + escritura atomicas
    if (ctx.r10.u32)
    {
        uint64_t o = __builtin_bswap64(*(uint64_t*)(g_base + ctx.r10.u32));
        if (o != 0xFFFFFFFFFFFFFFFEull) f->position = o;
    }
    LARGE_INTEGER li; li.QuadPart = (LONGLONG)f->position;
    SetFilePointerEx(f->host, li, nullptr, FILE_BEGIN);
    DWORD n = 0;
    WriteFile(f->host, g_base + ctx.r8.u32, ctx.r9.u32, &n, nullptr);
    f->position += n;
    SetIoStatus(ctx.r7.u32, XSTATUS_SUCCESS, n);
    SignalEvent(ctx.r4.u32);
    ctx.r3.u64 = XSTATUS_SUCCESS;
}
GUEST_IMPL(NtFlushBuffersFile) { if (auto f = GetFile(ctx.r3.u32)) if (!f->iso && f->host != INVALID_HANDLE_VALUE) FlushFileBuffers(f->host); ctx.r3.u64 = XSTATUS_SUCCESS; }
GUEST_IMPL(NtCancelIoFile) { ctx.r3.u64 = XSTATUS_SUCCESS; }

static uint64_t FileSize(FileObj& f)
{
    if (f.iso) return f.dir ? 0 : f.entry->size;
    if (f.dir) return 0;
    LARGE_INTEGER s{}; GetFileSizeEx(f.host, &s); return (uint64_t)s.QuadPart;
}

// NtQueryInformationFile(handle, io_status, info_ptr, length, info_class)
GUEST_IMPL(NtQueryInformationFile)
{
    auto f = GetFile(ctx.r3.u32);
    if (!f) { ctx.r3.u64 = XSTATUS_INVALID_HANDLE; return; }
    uint32_t p = ctx.r5.u32, info = 0;
    uint64_t size = FileSize(*f);
    switch (ctx.r7.u32)
    {
    case 5:  // FileStandardInformation {alloc u64, eof u64, links u32, delete_pending u8, directory u8}
        memset(g_base + p, 0, 0x18);
        Store64(p, (size + 0xFFF) & ~0xFFFull); Store64(p + 8, size); Store32(p + 0x10, 1); g_base[p + 0x15] = f->dir;
        info = 0x18; break;
    case 6:  // FileInternalInformation {u64 index_number}: identificador unico del archivo (NG2 lo pide al abrir los DLC, sesion 11)
        Store64(p, 0x1000000ull + ((uint64_t)(uintptr_t)f.get() >> 4)); info = 8; break;
    case 14: // FilePositionInformation
        Store64(p, f->position); info = 8; break;
    case 34: // FileNetworkOpenInformation
        WriteNetworkOpenInfo(p, size, f->dir); info = 0x38; break;
    case 4:  // FileBasicInformation {4 tiempos u64, attributes u32}
        WriteNetworkOpenInfo(p, size, f->dir); Store32(p + 0x20, f->dir ? 0x10 : 0x80); info = 0x28; break;
    default:
        LOG("[fs] NtQueryInformationFile clase %u no soportada", ctx.r7.u32);
        SetIoStatus(ctx.r4.u32, XSTATUS_INVALID_PARAMETER, 0); ctx.r3.u64 = XSTATUS_INVALID_PARAMETER; return;
    }
    SetIoStatus(ctx.r4.u32, XSTATUS_SUCCESS, info);
    ctx.r3.u64 = XSTATUS_SUCCESS;
}
// NtSetInformationFile(handle, io_status, info_ptr, length, info_class)
GUEST_IMPL(NtSetInformationFile)
{
    auto f = GetFile(ctx.r3.u32);
    if (!f) { ctx.r3.u64 = XSTATUS_INVALID_HANDLE; return; }
    uint32_t p = ctx.r5.u32;
    switch (ctx.r7.u32)
    {
    case 14: f->position = __builtin_bswap64(*(uint64_t*)(g_base + p)); break;
    case 20: // FileEndOfFileInformation
    case 19: // FileAllocationInformation
        if (!f->iso && f->host != INVALID_HANDLE_VALUE)
        {
            LARGE_INTEGER li; li.QuadPart = (LONGLONG)__builtin_bswap64(*(uint64_t*)(g_base + p));
            SetFilePointerEx(f->host, li, nullptr, FILE_BEGIN); SetEndOfFile(f->host);
        }
        break;
    default: LOG("[fs] NtSetInformationFile clase %u ignorada", ctx.r7.u32); break;
    }
    SetIoStatus(ctx.r4.u32, XSTATUS_SUCCESS, 0);
    ctx.r3.u64 = XSTATUS_SUCCESS;
}
// NtQueryFullAttributesFile(obj_attributes, network_open_info_ptr)
GUEST_IMPL(NtQueryFullAttributesFile)
{
    std::string rel, device;
    if (ResolvePath(ObjectPath(ctx.r3.u32), rel, device))
    {
        auto it = g_iso.find(rel);
        if (it == g_iso.end()) { ctx.r3.u64 = XSTATUS_OBJECT_NAME_NOT_FOUND; return; }
        WriteNetworkOpenInfo(ctx.r4.u32, it->second.dir ? 0 : it->second.size, it->second.dir);
    }
    else
    {
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (!GetFileAttributesExA(HostPath(device, rel).c_str(), GetFileExInfoStandard, &a)) { ctx.r3.u64 = XSTATUS_OBJECT_NAME_NOT_FOUND; return; }
        WriteNetworkOpenInfo(ctx.r4.u32, (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow, (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    }
    ctx.r3.u64 = XSTATUS_SUCCESS;
}
// NtQueryVolumeInformationFile(handle, io_status, info_ptr, length, info_class)
GUEST_IMPL(NtQueryVolumeInformationFile)
{
    uint32_t p = ctx.r5.u32, info = 0;
    switch (ctx.r7.u32)
    {
    case 1: // FileFsVolumeInformation {creation u64, serial u32, label_len u32, supports_objects u8, label}
        memset(g_base + p, 0, 0x18); Store32(p + 8, 0x12345678); info = 0x18; break;
    case 3: // FileFsSizeInformation {total_units u64, available_units u64, sectors_per_unit u32, bytes_per_sector u32}
        Store64(p, 0x100000); Store64(p + 8, 0x80000); Store32(p + 0x10, 1); Store32(p + 0x14, 0x1000); info = 0x18; break;
    case 5: // FileFsAttributeInformation {attributes u32, max_name_len u32, fs_name_len u32, fs_name}
    {
        auto f = GetFile(ctx.r3.u32);
        const char* fs = f && f->iso ? "GDFX" : "FATX";
        Store32(p, 0); Store32(p + 4, 255); Store32(p + 8, 4); memcpy(g_base + p + 12, fs, 4); info = 16; break;
    }
    default:
        LOG("[fs] NtQueryVolumeInformationFile clase %u no soportada", ctx.r7.u32);
        ctx.r3.u64 = XSTATUS_INVALID_PARAMETER; return;
    }
    SetIoStatus(ctx.r4.u32, XSTATUS_SUCCESS, info);
    ctx.r3.u64 = XSTATUS_SUCCESS;
}
// NtQueryDirectoryFile(handle, event, apc_routine, apc_context, io_status, info_ptr, length, file_name(ANSI_STRING*), restart_scan[pila])
// Devuelve una entrada por llamada (X_FILE_DIRECTORY_INFORMATION, cabecera de 0x40 bytes + nombre ANSI).
GUEST_IMPL(NtQueryDirectoryFile)
{
    auto f = GetFile(ctx.r3.u32);
    if (!f || !f->dir) { ctx.r3.u64 = XSTATUS_INVALID_HANDLE; return; }
    std::string pattern = ctx.r10.u32 ? ReadAnsiString(ctx.r10.u32) : std::string();
    if (!pattern.empty()) f->enumPattern = pattern;
    else pattern = f->enumPattern.empty() ? "*" : f->enumPattern;
    bool restart = Load32(ctx.r1.u32 + 0x54) != 0;
    if (restart) f->enumIndex = 0;
    { static int nl = 0; if (nl++ < 20) LOG("[fs] listar \"%s\" patron \"%s\" (iso %d, hijos %zu, indice %zu, reinicio %d)", f->path.c_str(), pattern.c_str(), (int)f->iso,
        f->iso ? g_iso[f->path].children.size() : (size_t)0, (size_t)f->enumIndex, (int)restart); }
    std::string name; uint64_t size = 0; bool isDir = false;
    for (;;)
    {
        if (f->iso)
        {
            auto& kids = g_iso[f->path].children;
            if (f->enumIndex >= kids.size()) break;
            auto& e = g_iso[kids[f->enumIndex++]];
            if (!GlobMatch(pattern.c_str(), e.name.c_str())) continue;
            name = e.name; size = e.size; isDir = e.dir;
        }
        else
        {
            if (f->enumIndex == 0)
            {
                f->hostList.clear();
                WIN32_FIND_DATAA fd; HANDLE fh = FindFirstFileA((f->path + "\\*").c_str(), &fd);
                if (fh != INVALID_HANDLE_VALUE) { do { if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..")) f->hostList.push_back(fd.cFileName); } while (FindNextFileA(fh, &fd)); FindClose(fh); }
            }
            if (f->enumIndex >= f->hostList.size()) break;
            name = f->hostList[f->enumIndex++];
            if (!GlobMatch(pattern.c_str(), name.c_str())) { name.clear(); continue; }
            WIN32_FILE_ATTRIBUTE_DATA a{}; GetFileAttributesExA((f->path + "\\" + name).c_str(), GetFileExInfoStandard, &a);
            size = (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow; isDir = (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        }
        break;
    }
    if (name.empty())
    {
        uint32_t st = f->enumIndex <= 1 && restart ? XSTATUS_NO_SUCH_FILE : XSTATUS_NO_MORE_FILES;
        SetIoStatus(ctx.r7.u32, st, 0); ctx.r3.u64 = st; return;
    }
    uint32_t p = ctx.r8.u32, need = 0x40 + (uint32_t)name.size();
    if (need > ctx.r9.u32) { ctx.r3.u64 = XSTATUS_BUFFER_TOO_SMALL; return; }
    memset(g_base + p, 0, 0x40);
    WriteNetworkOpenInfo(p + 8, size, isDir); // tiempos (4x u64) en +0x08, luego eof/alloc
    Store64(p + 0x28, size); Store64(p + 0x30, (size + 0xFFF) & ~0xFFFull);
    Store32(p + 0x38, isDir ? 0x10 : 0x80);
    Store32(p + 0x3C, (uint32_t)name.size());
    memcpy(g_base + p + 0x40, name.data(), name.size());
    Store32(p, 0); Store32(p + 4, (uint32_t)f->enumIndex);
    SetIoStatus(ctx.r7.u32, XSTATUS_SUCCESS, need);
    SignalEvent(ctx.r4.u32);
    ctx.r3.u64 = XSTATUS_SUCCESS;
}
// ObCreateSymbolicLink(link ANSI_STRING*, target ANSI_STRING*)
GUEST_IMPL(ObCreateSymbolicLink)
{
    std::string link = Lower(ReadAnsiString(ctx.r3.u32)), target = ReadAnsiString(ctx.r4.u32);
    if (link.compare(0, 4, "\\??\\") == 0) link = link.substr(4);
    while (!target.empty() && target.back() == '\\') target.pop_back();
    LOG("[fs] enlace %s -> %s", link.c_str(), target.c_str());
    std::lock_guard lock(g_linkMutex);
    g_links.emplace_back(link, target);
    ctx.r3.u64 = XSTATUS_SUCCESS;
}
// Contenido XAM (partidas guardadas, sesion 16): "<root>:" -> userdata\content\<tipo>_<archivo>\
// Devuelve 1 si la carpeta ya existia, 0 si se ha creado y -1 si no existe y no se pide crearla.
int FsMountContent(const std::string& root, const std::string& folder, bool create)
{
    std::string dev = "content\\" + Lower(folder);
    std::string host = g_dataDir + "\\" + dev;
    bool existed = GetFileAttributesA(host.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!existed && !create) return -1;
    CreateDirectoryA(g_dataDir.c_str(), nullptr);
    CreateDirectoryA((g_dataDir + "\\content").c_str(), nullptr);
    CreateDirectoryA(host.c_str(), nullptr);
    std::string link = Lower(root) + ":";
    std::lock_guard lock(g_linkMutex);
    g_links.erase(std::remove_if(g_links.begin(), g_links.end(), [&](auto& l) { return l.first == link; }), g_links.end());
    g_links.emplace_back(link, dev + ":");
    LOG("[fs] contenido %s -> %s (%s)", link.c_str(), host.c_str(), existed ? "existente" : "nuevo");
    return existed ? 1 : 0;
}
std::string FsContentDir() { return g_dataDir + "\\content"; }
void FsUnmountContent(const std::string& root)
{
    std::string link = Lower(root) + ":";
    std::lock_guard lock(g_linkMutex);
    g_links.erase(std::remove_if(g_links.begin(), g_links.end(), [&](auto& l) { return l.first == link; }), g_links.end());
}
GUEST_IMPL(ObDeleteSymbolicLink)
{
    std::string link = Lower(ReadAnsiString(ctx.r3.u32));
    if (link.compare(0, 4, "\\??\\") == 0) link = link.substr(4);
    std::lock_guard lock(g_linkMutex);
    g_links.erase(std::remove_if(g_links.begin(), g_links.end(), [&](auto& l) { return l.first == link; }), g_links.end());
    ctx.r3.u64 = XSTATUS_SUCCESS;
}
