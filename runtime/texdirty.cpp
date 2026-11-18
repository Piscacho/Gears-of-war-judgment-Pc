// GoWJ sesion 10 (del port de Gears 3, sesion 3): paginas sucias de la memoria fisica para saber si una textura ha cambiado.
// La huella muestreada (256 puntos) no veia el cambio en texturas casi vacias (fuentes, iconos del HUD con fondo transparente):
// dos imagenes distintas daban la misma huella y se seguia usando la vieja (letras hechas de trozos de otras, iconos rotos).
// TexDirtyArm protege (solo lectura) las paginas en las dos vistas de la memoria fisica (0xA0000000 y su espejo 0xC0000000); la
// primera escritura (de la CPU del juego o del runtime) cae en TexDirtyFault, que marca la pagina como sucia y la desprotege.
// Las lecturas de archivo / red que escriben en memoria fisica con APIs de Windows llaman antes a TexDirtyTouch (el kernel no
// genera excepcion: la llamada fallaria con ERROR_NOACCESS).
#include "runtime.h"
#include <windows.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <algorithm>

namespace
{
constexpr uint32_t kPages = 0x20000000 / 0x1000;   // 512 MB fisicos
std::vector<std::atomic<uint8_t>> g_state(kPages);  // 0 = sin vigilar, 1 = vigilada (protegida), 2 = sucia
// Contador de escrituras por pagina: cada consumidor (textura, bufer de vertices) guarda la suma de su rango al copiar. Con un solo
// bit "sucia" compartido, el primero que volvia a vigilar la pagina borraba la marca para los demas y estos seguian con la copia
// vieja (vertices del arma del jugador: arma cian translucida).
std::vector<std::atomic<uint32_t>> g_gen(kPages);
// Contador por grupo de 16 paginas (64 KB): cualquier escritura vigilada en el grupo lo incrementa. Las firmas se comprueban por
// grupos (16 veces menos lecturas para buferes grandes; puede dar alguna invalidacion de mas si se escribio otra pagina del grupo).
constexpr uint32_t kGroupShift = 4;
std::vector<std::atomic<uint32_t>> g_ggen(kPages >> kGroupShift);
inline void Bump(uint32_t p) { g_gen[p]++; g_ggen[p >> kGroupShift]++; }
std::mutex g_mx;
bool g_mirror = true;
std::atomic<uint64_t> g_faults{ 0 }, g_armed{ 0 }, g_touched{ 0 }, g_protFail{ 0 };
void ProtectRun(uint32_t p0, uint32_t n, DWORD prot)
{
    DWORD old;
    if (!VirtualProtect(g_base + 0xA0000000u + p0 * 0x1000u, size_t(n) * 0x1000, prot, &old)) g_protFail++;
    if (g_mirror && !VirtualProtect(g_base + 0xC0000000u + p0 * 0x1000u, size_t(n) * 0x1000, prot, &old)) g_protFail++;
}
bool Disabled() { static const bool off = getenv("GOWJ_NOTEXDIRTY") != nullptr; return off; }
// direccion de invitado -> pagina fisica (solo las vistas 0xA0000000-0xDFFFFFFF)
bool PhysRange(uint32_t addr, uint32_t len, uint32_t& p0, uint32_t& p1)
{
    if (!len) return false;
    uint32_t phys = addr & 0x1FFFFFFF;
    p0 = phys >> 12; p1 = (uint32_t)std::min<uint64_t>((uint64_t(phys) + len - 1) >> 12, kPages - 1);
    return true;
}
}

bool TexDirtyEnabled() { return !Disabled(); }
// true si alguna pagina del rango ha cambiado desde que se vigilo (o no estaba vigilada)
bool TexDirtyCheck(uint32_t phys, uint32_t len)
{
    uint32_t p0, p1; if (!PhysRange(phys, len, p0, p1)) return true;
    for (uint32_t p = p0; p <= p1; p++) if (g_state[p].load(std::memory_order_relaxed) != 1) return true;
    return false;
}
void TexDirtyArm(uint32_t phys, uint32_t len)
{
    if (Disabled()) return;
    static bool init = [] { volatile uint8_t* b = g_base; uint8_t o = b[0xA0000000u]; b[0xA0000000u] = o ^ 0x5A; g_mirror = b[0xC0000000u] == (uint8_t)(o ^ 0x5A); b[0xA0000000u] = o; return true; }();
    (void)init;
    uint32_t p0, p1; if (!PhysRange(phys, len, p0, p1)) return;
    std::lock_guard l(g_mx);
    uint32_t run = 0, start = 0;
    for (uint32_t p = p0; p <= p1 + 1; p++)
    {
        bool need = p <= p1 && g_state[p].load() != 1;
        if (need) { if (!run) start = p; run++; continue; }
        if (run)
        {   // primero proteger y despues marcar: una escritura entre medias (pagina ya "vigilada" pero aun escribible) se perdia
            // y la copia vieja seguia valiendo. Una escritura tras proteger espera al cerrojo y ve el estado 1.
            ProtectRun(start, run, PAGE_READONLY);
            for (uint32_t q = start; q < start + run; q++) g_state[q] = 1;
            g_armed += run; run = 0;
        }
    }
}
// Vigila el rango y devuelve su firma (suma de contadores de escritura). Tomarla antes de leer los datos.
uint64_t TexDirtyArmSig(uint32_t phys, uint32_t len)
{
    TexDirtyArm(phys, len);
    uint32_t p0, p1; if (!PhysRange(phys, len, p0, p1)) return 0;
    std::lock_guard l(g_mx);
    uint64_t sum = 0; for (uint32_t gi = p0 >> kGroupShift; gi <= (p1 >> kGroupShift); gi++) sum += g_ggen[gi].load();
    // Tras vigilar, todas las paginas del rango estan protegidas: cualquier escritura posterior pasa por Bump y cambia la suma
    return sum;
}
// true si nadie ha escrito en el rango desde que se tomo la firma (todas sus paginas siguen vigiladas y los contadores igual)
bool TexDirtyValid(uint32_t phys, uint32_t len, uint64_t sig)
{
    if (Disabled()) return false;
    uint32_t p0, p1; if (!PhysRange(phys, len, p0, p1)) return false;
    uint64_t sum = 0;
    for (uint32_t gi = p0 >> kGroupShift; gi <= (p1 >> kGroupShift); gi++) sum += g_ggen[gi].load(std::memory_order_acquire);
    return sum == sig;
}
// GoWJ sesion 11: firma exacta por pagina (sin agrupar) para los destinos de resolves por GPU: saber si la CPU ha escrito en esa
// memoria despues del resolve (el juego la ha reutilizado para otra cosa, p. ej. una textura recien cargada). Con grupos de 64 KB
// cualquier escritura vecina daria un falso aviso. Con el sistema desactivado devuelve 0 y la comprobacion siempre da "valida".
uint64_t TexDirtyPageSig(uint32_t addr, uint32_t len)
{
    if (Disabled()) return 0;
    TexDirtyArm(addr, len);
    uint32_t p0, p1; if (!PhysRange(addr, len, p0, p1)) return 0;
    std::lock_guard l(g_mx);
    uint64_t sum = 0; for (uint32_t p = p0; p <= p1; p++) sum += g_gen[p].load();
    return sum;
}
bool TexDirtyPageValid(uint32_t addr, uint32_t len, uint64_t sig)
{
    if (Disabled()) return true;
    uint32_t p0, p1; if (!PhysRange(addr, len, p0, p1)) return true;
    uint64_t sum = 0; for (uint32_t p = p0; p <= p1; p++) sum += g_gen[p].load(std::memory_order_acquire);
    return sum == sig;
}
// Antes de que una API de Windows escriba en memoria de invitado (ReadFile, recvfrom): desprotege y marca como sucias
void TexDirtyTouch(uint32_t addr, uint32_t len)
{
    if (Disabled() || addr < 0xA0000000u || addr >= 0xE0000000u) return;
    uint32_t p0, p1; if (!PhysRange(addr, len, p0, p1)) return;
    std::lock_guard l(g_mx);
    uint32_t run = 0, start = 0;
    for (uint32_t p = p0; p <= p1 + 1; p++)
    {
        bool watched = p <= p1 && g_state[p].load() == 1;
        if (watched) { if (!run) start = p; run++; g_state[p] = 2; Bump(p); continue; }
        if (run) { ProtectRun(start, run, PAGE_READWRITE); run = 0; }
    }
}
// Desde el manejador de excepciones: escritura en una direccion host. true si era una pagina vigilada (ya desprotegida).
bool TexDirtyFault(uintptr_t host)
{
    if (host < (uintptr_t)g_base + 0xA0000000u || host >= (uintptr_t)g_base + 0xE0000000ull) return false;
    uint32_t page = (uint32_t)((host - (uintptr_t)g_base) & 0x1FFFFFFF) >> 12;
    std::lock_guard l(g_mx);
    if (g_state[page].load() != 1)
    {   // otra hebra ya la desprotegio (carrera): reintentar si la vista ya es escribible; si sigue protegida sin estar vigilada
        // (no deberia pasar), se desprotege para no colgar al juego
        MEMORY_BASIC_INFORMATION mbi{};
        VirtualQuery((void*)(host & ~0xFFFull), &mbi, sizeof(mbi));
        if (mbi.Protect == PAGE_READWRITE) return true;
        if (mbi.Protect == PAGE_READONLY) { g_state[page] = 2; Bump(page); ProtectRun(page, 1, PAGE_READWRITE); return true; }
        return false;
    }
    g_state[page] = 2; Bump(page); ProtectRun(page, 1, PAGE_READWRITE); g_faults++;
    {   // diagnostico (GoWJ sesion 11): GOWJ_TDWATCH=<fisica hex>,<largo hex>: registra las escrituras capturadas en ese rango
        static uint32_t lo = 0, hi = 0; static bool init = [] { if (const char* s = getenv("GOWJ_TDWATCH")) { char* e; lo = strtoul(s, &e, 16) & 0x1FFFFFFF; hi = lo + (*e == ',' ? strtoul(e + 1, nullptr, 16) : 0x1000); } return true; }(); (void)init;
        uint32_t ph = page << 12;
        if (ph >= (lo & ~0xFFFu) && ph < hi) LOG("[tdwatch] escritura capturada en la pagina %08X (hilo %lu)", ph, GetCurrentThreadId());
    }
    return true;
}
// Estadistica (sesion 10): paginas protegidas, fallos de escritura capturados y fallos de VirtualProtect desde la ultima llamada
void TexDirtyStatsLog()
{
    static uint64_t la = 0, lf = 0, lp = 0;
    uint64_t a = g_armed, f = g_faults, p = g_protFail;
    LOG("[texdirty] paginas protegidas %llu, escrituras capturadas %llu, VirtualProtect fallidos %llu (espejo %d)", (unsigned long long)(a - la), (unsigned long long)(f - lf), (unsigned long long)(p - lp), (int)g_mirror);
    la = a; lf = f; lp = p;
}
