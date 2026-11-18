// Ganchos de diagnostico sobre funciones del juego (NG2). Las funciones traducidas son "weak": definirlas aqui las envuelve.
// GOWJ_HOOKLOG=1 registra cada llamada (con hilo, r3..r5 y lr) de las funciones de la lista.
#include "runtime.h"
#include <ppc_recomp_shared.h>
#include "tu_remap.h"   // sesion 26: nombres de la actualizacion del titulo
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <string>

namespace
{
const bool g_hookLog = getenv("GOWJ_HOOKLOG") != nullptr;
void HookLog(const char* name, PPCContext& ctx, std::atomic<int>& n)
{
    if (!g_hookLog) return;
    int k = n++;
    if (k < 20 || (k % 1000) == 0)
        LOG("[hook] %s #%d tid %lu r3=%08X r4=%08X r5=%08X lr=%08X", name, k, GetCurrentThreadId(), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, (uint32_t)ctx.lr);
}
}

#define GOWJ_TRACE_HOOK(addr, label) \
    PPC_FUNC_IMPL(__imp__sub_##addr); \
    PPC_FUNC(sub_##addr) { static std::atomic<int> n{ 0 }; HookLog(label, ctx, n); __imp__sub_##addr(ctx, base); }

// Gancho que registra el valor devuelto (r3) cada vez que cambia
#define GOWJ_RET_HOOK(addr, label)     PPC_FUNC_IMPL(__imp__sub_##addr);     PPC_FUNC(sub_##addr) { static std::atomic<uint64_t> last{ ~0ull }; uint32_t a3 = ctx.r3.u32; __imp__sub_##addr(ctx, base);         if (g_hookLog && last.exchange(ctx.r3.u64) != ctx.r3.u64) LOG("[ret] %s(r3=%08X) -> %llX (t=%lu ms)", label, a3, (unsigned long long)ctx.r3.u64, GetTickCount()); }
// GoW Judgment: aun sin ganchos propios (los de NG2 eran direcciones de NG2).

// Sesion 2: busqueda en TMap<FString,...> (sub_822B48E8: r3 = salida, r4 = mapa, r5 = FString clave). Mapa: +0 elementos (0x1C bytes,
// siguiente en +0x14), +0x30 hash en linea, +0x34 puntero al hash, +0x38 tamano del hash. GOWJ_MAPCHECK=1 detecta ciclos y vuelca.
#include <set>
PPC_FUNC_IMPL(__imp__sub_822B48E8);
PPC_FUNC(sub_822B48E8)
{
    static const bool check = getenv("GOWJ_MAPCHECK") != nullptr;
    if (check)
    {
        uint32_t m = ctx.r4.u32, elems = Load32(m), hsize = Load32(m + 0x38), hptr = Load32(m + 0x34);
        uint32_t hash = hptr ? hptr : m + 0x30;
        static std::atomic<int> dumps{ 0 };
        for (uint32_t b = 0; b < hsize && dumps < 3; b++)
        {
            std::set<uint32_t> seen; uint32_t i = Load32(hash + b * 4), steps = 0;
            while (i != 0xFFFFFFFF && steps < 100000) { if (!seen.insert(i).second) break; i = Load32(elems + i * 0x1C + 0x14); steps++; }
            if (i != 0xFFFFFFFF)
            {
                dumps++;
                LOG("[map] CICLO en mapa %08X cubo %u (hash %u cubos en %08X, elementos %08X, num %d max %d): vuelve a %u tras %u pasos",
                    m, b, hsize, hash, elems, (int)Load32(m + 4), (int)Load32(m + 8), i, steps);
                uint32_t j = Load32(hash + b * 4);
                for (int k = 0; k < 12; k++) { LOG("[map]   elem %u: %08X %08X %08X %08X %08X %08X %08X", j, Load32(elems + j * 0x1C), Load32(elems + j * 0x1C + 4), Load32(elems + j * 0x1C + 8),
                    Load32(elems + j * 0x1C + 12), Load32(elems + j * 0x1C + 16), Load32(elems + j * 0x1C + 20), Load32(elems + j * 0x1C + 24)); j = Load32(elems + j * 0x1C + 0x14); if (j == 0xFFFFFFFF) break; }
                for (int k = 0; k < 0x40; k += 4) LOG("[map]   mapa+%02X = %08X", k, Load32(m + k));
                break;
            }
        }
    }
    static const bool keyLog = getenv("GOWJ_TOCLOG") != nullptr;
    uint32_t keyStr = ctx.r5.u32, outp = ctx.r3.u32, mapArg = ctx.r4.u32;
    __imp__sub_822B48E8(ctx, base);
    if (keyLog && keyStr)
    {   // GoWJ sesion 6: busquedas en mapas FString (TOC de paquetes); se registran las que contienen GOWJ_TOCLOG
        uint32_t data = Load32(keyStr); std::string s;
        for (int i = 0; data && i < 200; i++) { uint16_t c = __builtin_bswap16(*(uint16_t*)(g_base + data + i * 2)); if (!c) break; s += c < 128 ? (char)c : '?'; }
        static std::string filt = getenv("GOWJ_TOCLOG");
        static std::atomic<int> n{ 0 };
        static const DWORD t0toc = GetTickCount();
        if (s.find(filt) != std::string::npos && GetTickCount() - t0toc > 40000 && n++ < 60)
        {
            LOG("[toc] busqueda '%s' -> %08X (t=%lu ms, tid %lu)", s.c_str(), Load32(outp), GetTickCount(), GetCurrentThreadId());
            {   char chain[300]; int c = 0; uint32_t sp = ctx.r1.u32;
                for (int k = 0; k < 24 && sp; k++) { uint32_t up = Load32(sp); if (!up || up <= sp) break; c += snprintf(chain + c, sizeof(chain) - c, " %08X", Load32(up - 8)); sp = up; }
                LOG("[toc]   pila:%s", chain); }
            static std::atomic<int> dumped{ 0 };
            uint32_t m = ctx.r4.u32;   // (r4 ya no es fiable tras la llamada; se registra el mapa global del TOC)
            if (dumped++ < 2)
            {
                uint32_t mm = mapArg, elems = Load32(mm), num = Load32(mm + 4), hsize = Load32(mm + 0x38), hptr = Load32(mm + 0x34);
                int found = -1; std::string sample;
                for (uint32_t i = 0; i < num && i < 5000; i++)
                {
                    uint32_t kp = Load32(elems + i * 0x1C); std::string k;
                    for (int j = 0; kp && j < 200; j++) { uint16_t c = __builtin_bswap16(*(uint16_t*)(g_base + kp + j * 2)); if (!c) break; k += c < 128 ? (char)c : '?'; }
                    if (i < 3) sample += " [" + k + "]";
                    if (k == s) found = (int)i;
                }
                LOG("[toc] mapa %08X: %u elementos, hash %u cubos (%08X), clave en elemento %d; ejemplos:%s", mm, num, hsize, hptr, found, sample.c_str());
                if (found >= 0) { uint32_t e = elems + found * 0x1C; LOG("[toc]   elemento: %08X %08X %08X %08X %08X %08X %08X", Load32(e), Load32(e+4), Load32(e+8), Load32(e+12), Load32(e+16), Load32(e+20), Load32(e+24)); }
            }
            (void)m;
        }
    }
}

// Sesion 2: TArray::Realloc (sub_822B1D10) -> GMalloc (0x83523438)->Realloc (vtable +0xC). GOWJ_REALLOCLOG=1 registra la funcion.
PPC_FUNC_IMPL(__imp__sub_822B1D10);
PPC_FUNC(sub_822B1D10)
{
    static const bool lg = getenv("GOWJ_REALLOCLOG") != nullptr;
    static std::atomic<int> n{ 0 };
    uint32_t arr = ctx.r3.u32, old = Load32(arr), cnt = ctx.r4.u32, sz = ctx.r5.u32;
    if (lg && (n++ < 3 || arr == 0x8355CF48))
    {
        uint32_t gm = Load32(0x83523438);
        LOG("[realloc] arr %08X viejo %08X n %u x %u, GMalloc %08X Realloc %08X interno %08X Realloc %08X", arr, old, cnt, sz, gm, gm ? Load32(Load32(gm) + 0xC) : 0, gm ? Load32(gm + 4) : 0, (gm && Load32(gm + 4)) ? Load32(Load32(Load32(gm + 4)) + 0xC) : 0);
    }
    __imp__sub_822B1D10(ctx, base);
    if (lg && arr == 0x8355CF48) LOG("[realloc] mapa TOC: %08X -> %08X (n %u x %u), primeras palabras viejo %08X nuevo %08X", old, Load32(arr), cnt, sz, old ? Load32(old) : 0, Load32(Load32(arr)));
}

// Sesion 2: manejador de error fatal de UE3 (sub_82321758: "appError called: " + mensaje en r4, cadena ancha) -> se registra el mensaje.
static std::string GuestWide(uint32_t p, size_t max = 2000)
{
    std::string s; if (!p) return s;
    for (size_t i = 0; i < max; i++) { uint16_t c = __builtin_bswap16(*(uint16_t*)(g_base + p + i * 2)); if (!c) break; s += c < 128 ? (char)c : '?'; }
    return s;
}
PPC_FUNC_IMPL(__imp__sub_82321758);
PPC_FUNC(sub_82321758)
{
    LOG("[appError] %s | %s (lr=%08X, tid %lu)", GuestWide(ctx.r4.u32).c_str(), GuestWide(ctx.r5.u32, 200).c_str(), (uint32_t)ctx.lr, GetCurrentThreadId());
    __imp__sub_82321758(ctx, base);
}

// Sesion 5: hilo de entrada (sub_8299C980: r3 = objeto; +0x1C lock, +4 anillo de muestras de 0x54 bytes, +0xC escritura, +0x14 tamano).
// GOWJ_INPUTLOG=1: registra el objeto y, cuando hay stick, la muestra que queda en el anillo.
PPC_FUNC_IMPL(__imp__sub_8299C980);
PPC_FUNC(sub_8299C980)
{
    static const bool lg = getenv("GOWJ_INPUTLOG") != nullptr;
    uint32_t o = ctx.r3.u32;
    __imp__sub_8299C980(ctx, base);
    {   // GOWJ_WATCHRING=1: vigila las lecturas del anillo (con GOWJ_WATCHREAD=1)
        static bool armed = false;
        if (!armed && getenv("GOWJ_WATCHRING")) { armed = true; void WatchArm(uint32_t, uint32_t); WatchArm(Load32(o + 4), Load32(o + 0x14) * 0x54); }
    }
    if (!lg) return;
    static int n = 0;
    uint32_t arr = Load32(o + 4), wr = Load32(o + 0xC), sz = Load32(o + 0x14), rd = Load32(o + 8);
    uint32_t idx = wr ? wr - 1 : sz - 1, e = arr + idx * 0x54;
    int16_t lx = (int16_t)(Load32(e + 0x10 + 0) >> 16);   // muestra: +0 tiempo (float), +4.. estados de 0x10 (paquete, botones...)
    bool stick = false;
    for (int u = 0; u < 4; u++) { uint32_t s = e + 0x14 + u * 0x10; if (Load32(s + 8) || Load32(s + 12)) stick = true; }
    if (n < 3 || (stick && n < 40)) { n++;
        LOG("[inputlog] obj %08X anillo %08X lect %u escr %u tam %u | muestra %08X: %08X %08X %08X %08X %08X %08X conectados %08X %08X", o, arr, rd, wr, sz, e,
            Load32(e), Load32(e + 4), Load32(e + 0x14), Load32(e + 0x18), Load32(e + 0x1C), Load32(e + 0x20), Load32(e + 0x24), Load32(e + 0x28)); }
    (void)lx;
}

// Sesion 5: consumidor del anillo de entrada (sub_8299C3D0: r3 = muestra combinada de 0x54 bytes). GOWJ_INPUTLOG=1 registra la salida.
PPC_FUNC_IMPL(__imp__sub_8299C3D0);
PPC_FUNC(sub_8299C3D0)
{
    static const bool lg = getenv("GOWJ_INPUTLOG") != nullptr;
    uint32_t out = ctx.r3.u32;
    __imp__sub_8299C3D0(ctx, base);
    if (!lg) return;
    static int n = 0;
    bool stick = Load32(out + 0x1C) || Load32(out + 0x20);
    static int calls = 0; calls++;
    if ((stick && n < 30) || n < 2 || (calls % 300 == 0 && calls < 30000)) { n++;
        LOG("[inputout] r3=%u | %08X %08X | %08X %08X %08X %08X", ctx.r3.u32, Load32(out), Load32(out + 4), Load32(out + 0x14), Load32(out + 0x18), Load32(out + 0x1C), Load32(out + 0x20)); }
}

// Sesion 5: procesado de entrada por cuadro (sub_829A18C8: f1 = DeltaTime). GOWJ_INPUTLOG=1 registra f1 de vez en cuando.
PPC_FUNC_IMPL(__imp__sub_829A18C8);
PPC_FUNC(sub_829A18C8)
{
    static const bool lg = getenv("GOWJ_INPUTLOG") != nullptr;
    static int n = 0;
    if (lg && (n++ % 200) == 0 && n < 4000) LOG("[inputdt] f1=%g f2=%g r3=%08X", ctx.f1.f64, ctx.f2.f64, ctx.r3.u32);
    __imp__sub_829A18C8(ctx, base);
}

// Sesion 6: funciones nativas de viaje/cambio de mapa de AWorldInfo (exec: r3 = this, r4 = FFrame, r5 = resultado).
// GOWJ_TRAVELLOG=1 registra cada llamada (las primeras 40 de cada una).
#define GOWJ_EXEC_HOOK(addr, label) \
    PPC_FUNC_IMPL(__imp__sub_##addr); \
    PPC_FUNC(sub_##addr) { static const bool lg = getenv("GOWJ_TRAVELLOG") != nullptr; static std::atomic<int> n{ 0 }; uint32_t res = ctx.r5.u32; \
        __imp__sub_##addr(ctx, base); \
        if (lg && n++ < 40) LOG("[travel] %s (this %08X) resultado %08X tid %lu", label, ctx.r3.u32, res ? Load32(res) : 0, GetCurrentThreadId()); }
GOWJ_EXEC_HOOK(82548678, "SeamlessTravel")
GOWJ_EXEC_HOOK(825487D8, "IsInSeamlessTravel")
GOWJ_EXEC_HOOK(825487F8, "SetSeamlessTravelMidpointPause")
GOWJ_EXEC_HOOK(82548618, "CommitMapChange")
GOWJ_EXEC_HOOK(82548448, "PrepareMapChange")
GOWJ_EXEC_HOOK(82548528, "IsPreparingMapChange")
GOWJ_EXEC_HOOK(82548568, "IsMapChangeReady")

// Sesion 6: URL de los viajes (UGameEngine::Browse = sub_82559608?, LoadMap = sub_8255A5E0?). FURL: Protocol +0, Host +0xC, Port +0x18,
// Map +0x1C, Op (TArray<FString>) +0x28, Portal +0x34. GOWJ_TRAVELLOG=1 registra la URL de r4 (y r5) de cada llamada.
static std::string GuestFStr(uint32_t fs)
{
    std::string s; if (!fs) return s;
    uint32_t d = Load32(fs); int n = (int)Load32(fs + 4);
    if (!d || n <= 0 || n > 512) return s;
    for (int i = 0; i < n; i++) { uint16_t c = __builtin_bswap16(*(uint16_t*)(g_base + d + i * 2)); if (!c) break; s += c < 128 ? (char)c : '?'; }
    return s;
}
static void LogUrl(const char* tag, uint32_t u)
{
    if (!u) return;
    std::string ops; uint32_t od = Load32(u + 0x28); int on = (int)Load32(u + 0x2C);
    for (int i = 0; od && i < on && i < 16; i++) ops += "?" + GuestFStr(od + i * 12);
    LOG("[travel] %s: proto '%s' host '%s' port %d mapa '%s' ops '%s' portal '%s'", tag, GuestFStr(u).c_str(), GuestFStr(u + 0xC).c_str(), (int)Load32(u + 0x18),
        GuestFStr(u + 0x1C).c_str(), ops.c_str(), GuestFStr(u + 0x34).c_str());
}
PPC_FUNC_IMPL(__imp__sub_82559608);
PPC_FUNC(sub_82559608) { if (getenv("GOWJ_TRAVELLOG")) { LogUrl("Browse r4", ctx.r4.u32); } __imp__sub_82559608(ctx, base); }
PPC_FUNC_IMPL(__imp__sub_8255A5E0);
PPC_FUNC(sub_8255A5E0) { if (getenv("GOWJ_TRAVELLOG")) { LogUrl("LoadMap r4", ctx.r4.u32); } __imp__sub_8255A5E0(ctx, base); }
GOWJ_EXEC_HOOK(82AF4A90, "GearEngine.SaveCheckpoint")
GOWJ_EXEC_HOOK(82AF4B18, "GearEngine.LoadCheckpoint")
GOWJ_EXEC_HOOK(82AF4B28, "GearEngine.SetCheckpoint")
GOWJ_EXEC_HOOK(82AF4C38, "GearEngine.ParseCheckpointData")
GOWJ_EXEC_HOOK(82AE5588, "GearPC_SP.LogSinglePlayerChapterDataInternal")
GOWJ_EXEC_HOOK(824E5568, "SeqAct_WaitForLevelsVisible.CheckLevelsVisible")
GOWJ_EXEC_HOOK(82AF3970, "GearGameViewportClient.IsTransitionLevel")
#define GOWJ_EXEC_HOOK_STR(addr, label) \
    PPC_FUNC_IMPL(__imp__sub_##addr); \
    PPC_FUNC(sub_##addr) { static const bool lg = getenv("GOWJ_TRAVELLOG") != nullptr; static std::atomic<int> n{ 0 }; uint32_t res = ctx.r5.u32; \
        __imp__sub_##addr(ctx, base); \
        if (lg && n++ < 40) LOG("[travel] %s -> '%s' (%08X)", label, GuestFStr(res).c_str(), res ? Load32(res) : 0); }
GOWJ_EXEC_HOOK_STR(82AF4590, "GearGame.GetMapNameOverride")
GOWJ_EXEC_HOOK_STR(82AF4738, "GearGame.GetMapFilename")
GOWJ_EXEC_HOOK(82AF4548, "GearGame.IsSmokeTest")
GOWJ_EXEC_HOOK(82AF4508, "GearGame.HavePendingConnections")
GOWJ_EXEC_HOOK(82AF4378, "GearGameSP_Base.TellClientToPlayMovieAfterLogon")
GOWJ_EXEC_HOOK(82AF4EA0, "GearEngine.CheckCIV")
GOWJ_EXEC_HOOK(82AF49E0, "GearEngine.LoadGameTypeConfigs")
GOWJ_EXEC_HOOK(82AF4A70, "GearEngine.InvalidateLastLoadedGameType")
GOWJ_EXEC_HOOK(82AF4F50, "GearEngine.IsCookedBuild")

// Sesion 6: registro generico de llamadas (GOWJ_TRAVELLOG=1): argumentos y pila de invitado de las primeras llamadas
static void CallLog(const char* label, PPCContext& ctx, std::atomic<int>& n)
{
    static const bool lg = getenv("GOWJ_TRAVELLOG") != nullptr;
    if (!lg || n++ >= 6) return;
    char chain[200]; int c = 0; uint32_t sp = ctx.r1.u32;
    for (int k = 0; k < 10 && sp; k++) { uint32_t up = Load32(sp); if (!up || up <= sp) break; c += snprintf(chain + c, sizeof(chain) - c, " %08X", Load32(up - 8)); sp = up; }
    LOG("[call] %s r3=%08X r4=%08X r5=%08X r6=%08X lr=%08X pila:%s", label, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, (uint32_t)ctx.lr, chain);
}
#define GOWJ_CALL_HOOK(addr, label) \
    PPC_FUNC_IMPL(__imp__sub_##addr); \
    PPC_FUNC(sub_##addr) { static std::atomic<int> n{ 0 }; CallLog(label, ctx, n); __imp__sub_##addr(ctx, base); }
GOWJ_CALL_HOOK(8255C978, "PrepareMapChange nativo (LevelStreamingPersistent_%s)")
PPC_FUNC_IMPL(__imp__sub_82557748);
PPC_FUNC(sub_82557748)
{
    static const bool lg = getenv("GOWJ_TRAVELLOG") != nullptr;
    static std::atomic<int> n{ 0 };
    if (lg && n++ < 30) LOG("[call] StreamByURL '%s' r5=%08X r6=%08X", GuestWide(ctx.r4.u32, 300).c_str(), ctx.r5.u32, ctx.r6.u32);
    __imp__sub_82557748(ctx, base);
    if (lg && n <= 30) LOG("[call]   -> %08X", ctx.r3.u32);
}
GOWJ_CALL_HOOK(82BA6180, "Main_Sequence #1")
GOWJ_CALL_HOOK(82BAC3A8, "Main_Sequence #2")
GOWJ_CALL_HOOK(82BB77F0, "Main_Sequence #3")
GOWJ_CALL_HOOK(82BABB80, "Chapters")

// Sesion 7: peticion de carga asincrona de paquetes (sub_822BAF00 = UObject::LoadPackageAsync?). GOWJ_ASYNCLOG=1 registra nombre y pila.
PPC_FUNC_IMPL(__imp__sub_822BAF00);
PPC_FUNC(sub_822BAF00)
{
    static const bool lg = getenv("GOWJ_ASYNCLOG") != nullptr;
    if (lg)
    {
        static std::atomic<int> n{ 0 };
        if (n++ < 80)
        {
            char chain[200]; int c = 0; uint32_t sp = ctx.r1.u32;
            for (int k = 0; k < 10 && sp; k++) { uint32_t up = Load32(sp); if (!up || up <= sp) break; c += snprintf(chain + c, sizeof(chain) - c, " %08X", Load32(up - 8)); sp = up; }
            auto A = [](uint32_t p) { std::string s; for (int i = 0; p && i < 120; i++) { char ch = (char)g_base[p + i]; if (!ch) break; s += (ch >= 32 && ch < 127) ? ch : '?'; } return s; };
            LOG("[async] r3 %08X '%s' r4 %08X '%s' r5=%08X r6=%08X pila:%s", ctx.r3.u32, A(ctx.r3.u32).c_str(), ctx.r4.u32, GuestWide(ctx.r4.u32, 120).c_str(), ctx.r5.u32, ctx.r6.u32, chain);
        }
    }
    __imp__sub_822BAF00(ctx, base);
}
GOWJ_CALL_HOOK(8255C638, "async@8255C638")
GOWJ_CALL_HOOK(8270B728, "async@8270B728")
GOWJ_CALL_HOOK(8270DAF8, "async@8270DAF8")
GOWJ_CALL_HOOK(8270DE20, "async@8270DE20")
GOWJ_CALL_HOOK(82A8ACD8, "async@82A8ACD8")
GOWJ_CALL_HOOK(82A8B068, "async@82A8B068")
GOWJ_CALL_HOOK(82A8E5D8, "async@82A8E5D8")
GOWJ_CALL_HOOK(82A8E880, "async@82A8E880")
GOWJ_CALL_HOOK(82A8F028, "async@82A8F028")
GOWJ_CALL_HOOK(82A8F3B0, "async@82A8F3B0")
GOWJ_CALL_HOOK(82B61F90, "async@82B61F90")
GOWJ_CALL_HOOK(82708100, "async@82708100")

// Sesion 7: UGearEngine::LoadMap? (sub_82A69928) decide si pide el mapa del capitulo (sub_8255C638 + sub_8255C978) a partir de
// ?chapter=. Registro de las llamadas hechas desde ella (lr dentro de la funcion) con su resultado. GOWJ_TRAVELLOG=1.
static bool LrInLoadMap(PPCContext& ctx) { uint32_t lr = (uint32_t)ctx.lr; return lr > 0x82A69928 && lr < 0x82A6A1C0; }
#define GOWJ_LR_HOOK(addr, label, WIDE) \
    PPC_FUNC_IMPL(__imp__sub_##addr); \
    PPC_FUNC(sub_##addr) { static const bool lg = getenv("GOWJ_TRAVELLOG") != nullptr; \
        if (!lg || !LrInLoadMap(ctx)) { __imp__sub_##addr(ctx, base); return; } \
        uint32_t a3 = ctx.r3.u32, a4 = ctx.r4.u32, a5 = ctx.r5.u32, lr = (uint32_t)ctx.lr; \
        std::string s3 = WIDE ? GuestWide(a3, 100) : std::string(), s4 = WIDE ? GuestWide(a4, 100) : std::string(); \
        __imp__sub_##addr(ctx, base); \
        LOG("[loadmap] %s lr=%08X r3=%08X '%s' r4=%08X '%s' r5=%08X -> %08X", label, lr, a3, s3.c_str(), a4, s4.c_str(), a5, ctx.r3.u32); }
GOWJ_LR_HOOK(8270FAC8, "8270FAC8(hayCheckpoint?)", false)
GOWJ_LR_HOOK(82701FB8, "ParseOption", true)
GOWJ_LR_HOOK(82ACF600, "82ACF600(datos capitulos?)", false)
GOWJ_LR_HOOK(822B0CB8, "822B0CB8(comparar)", false)
GOWJ_LR_HOOK(82D9B090, "atoi", true)
GOWJ_LR_HOOK(82DA09F8, "wcsicmp", true)
PPC_FUNC_IMPL(__imp__sub_82A69928);
PPC_FUNC(sub_82A69928)
{
    static const bool lg = getenv("GOWJ_TRAVELLOG") != nullptr;
    { static bool once = false; if (!once) { once = true; uint32_t vt = Load32(ctx.r3.u32); LOG("[engine] GEngine %08X vtable %08X GetMaxTickRate(+0x134) = %08X", ctx.r3.u32, vt, Load32(vt + 0x134)); } }
    if (lg) { LOG("[loadmap] entra r3=%08X r4=%08X r5=%08X r6=%08X lr=%08X", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, (uint32_t)ctx.lr); LogUrl("GearLoadMap r4", ctx.r4.u32); }
    __imp__sub_82A69928(ctx, base);
    if (lg) LOG("[loadmap] sale -> %08X", ctx.r3.u32);
}
// Sesion 7: UWorld::Listen (sub_8270D3C8: r3 = FURL, r4 = FString& Error). Crea el NetDriver (GEngine vtable+0x180) y llama a
// InitListen (vtable+0x13C). GOWJ_TRAVELLOG=1 activa la traza de imports durante la llamada y registra el resultado.
extern bool g_importTrace;
PPC_FUNC_IMPL(__imp__sub_8270D3C8);
PPC_FUNC(sub_8270D3C8)
{
    static const bool lg = getenv("GOWJ_TRAVELLOG") != nullptr;
    uint32_t err = ctx.r4.u32; bool old = g_importTrace;
    if (lg) { LOG("[listen] entra URL '%s' lr=%08X", GuestFStr(ctx.r3.u32 + 0x1C).c_str(), (uint32_t)ctx.lr); g_importTrace = true; }
    __imp__sub_8270D3C8(ctx, base);
    if (lg) { g_importTrace = old; LOG("[listen] -> %u error '%s'", ctx.r3.u32, GuestFStr(err).c_str()); }
}

// Sesion 7: caida intermitente en sub_826A8DF8 (animacion; recorre una lista de objetos UObject enlazada por +0x90 desde
// *(*(r7+0x124) + idx*12 + 8), idx = byte de la TArray r3+0x348/0x354 en la posicion r4). A veces un puntero de la lista es texto
// UTF-16 (0x00470046) -> llamada virtual a NULL. GOWJ_ANIMCHECK=1 recorre la lista al entrar y registra el primer nodo invalido;
// GOWJ_ANIMFIX=1 ademas corta la lista en ese nodo (evita la caida).
static bool GuestObjOk(uint32_t p)
{
    if (p < 0x00100000 || (p >= 0x40000000 && p < 0x40000000) || p >= 0xA0000000 || (p & 3)) return false;
    uint32_t vt = Load32(p);
    return vt >= 0x82000000 && vt < 0x83700000;
}
#include <unordered_map>
#include <mutex>
// InitSkelControls (sub_826B0F70, r3 = USkeletalMeshComponent): construye SkelControlIndex (+0x348) y PostPhysSkelControlIndex (+0x354)
// a partir de Animations (+0x1FC, UAnimTree con SkelControlLists en +0x124/+0x128). Se registra con que arbol se construyo.
struct SkelInit { uint32_t tree, lists, num348, frame; };
static std::mutex g_skelMutex; static std::unordered_map<uint32_t, SkelInit> g_skelInit; static std::atomic<uint32_t> g_skelInitCount{ 0 };
PPC_FUNC_IMPL(__imp__sub_826B0F70);
PPC_FUNC(sub_826B0F70)
{
    uint32_t comp = ctx.r3.u32;
    __imp__sub_826B0F70(ctx, base);
    static const bool on = getenv("GOWJ_ANIMCHECK") != nullptr;
    if (on)
    {
        uint32_t tree = Load32(comp + 0x1FC);
        std::lock_guard<std::mutex> l(g_skelMutex);
        g_skelInit[comp] = { tree, tree ? Load32(tree + 0x128) : 0, Load32(comp + 0x34C), g_skelInitCount++ };
        uint32_t n = Load32(comp + 0x34C), m = Load32(comp + 0x348);
        if (n && n <= 8)
        {
            char mb[64]; int mc = 0; for (uint32_t i = 0; i < n; i++) mc += snprintf(mb + mc, sizeof(mb) - mc, "%u ", g_base[m + i]);
            LOG("[animinit] comp %08X arbol %08X listas %u mapa %08X (%u): %s lr %08X tid %lu", comp, tree, tree ? Load32(tree + 0x128) : 0, m, n, mb, (uint32_t)ctx.lr, GetCurrentThreadId());
            static std::atomic<int> armed{ 0 };
            extern void WatchArm(uint32_t guest, uint32_t len);
            if (getenv("GOWJ_ANIMWATCH") && n == 3 && armed++ == 0) { _putenv("GOWJ_WATCHFILTER="); char f[40]; snprintf(f, sizeof(f), "GOWJ_WATCHFILTER=%X,%X", m, n); _putenv(f); WatchArm(m, n); }
        }
    }
}
PPC_FUNC_IMPL(__imp__sub_826A8DF8);
PPC_FUNC(sub_826A8DF8)
{
    static const bool check = getenv("GOWJ_ANIMCHECK") != nullptr, fix = getenv("GOWJ_ANIMFIX") != nullptr;
    if (check || fix)
    {
        uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32, r6 = ctx.r6.u32, r7 = ctx.r7.u32;
        uint32_t arr = (r5 == 0 && r6 != 0) ? r3 + 0x354 : r3 + 0x348;
        int cnt = (int)Load32(arr + 4);
        if (cnt > 0 && r7)
        {
            uint32_t idx = g_base[Load32(arr) + r4];
            if (idx != 0xFF)
            {
                uint32_t tab = Load32(r7 + 0x124), slot = tab + idx * 12 + 8, prev = 0, p = Load32(slot);
                for (int k = 0; p && k < 4096; k++)
                {
                    if (!GuestObjOk(p))
                    {
                        static std::atomic<int> n{ 0 };
                        if (n++ < 20)
                        {
                            char hex[3 * 64 + 1]; int c = 0; uint32_t at = prev ? prev + 0x60 : slot - 8;
                            for (int i = 0; i < 64; i++) c += snprintf(hex + c, sizeof(hex) - c, "%02X ", g_base[at + i]);
                            LOG("[animcheck] nodo invalido %08X (k=%d) prev %08X (vt %08X) cabeza %08X tab %08X (num %d max %d) idx %u mapa %08X num %d r3 %08X (vt %08X) r4 %u r5 %08X r6 %08X r7 %08X (vt %08X) lr %08X tid %lu",
                                p, k, prev, prev ? Load32(prev) : 0, slot, tab, (int)Load32(r7 + 0x128), (int)Load32(r7 + 0x12C), idx, Load32(arr), cnt, r3, Load32(r3), r4, r5, r6, r7, Load32(r7), (uint32_t)ctx.lr, GetCurrentThreadId());
                            LOG("[animcheck]   memoria en %08X: %s", at, hex);
                            SkelInit si{}; bool have; { std::lock_guard<std::mutex> l(g_skelMutex); auto it = g_skelInit.find(r3); have = it != g_skelInit.end(); if (have) si = it->second; }
                            uint32_t m = Load32(arr); char mb[64]; int mc = 0; for (int i = 0; i < cnt && i < 16; i++) mc += snprintf(mb + mc, sizeof(mb) - mc, "%u ", g_base[m + i]);
                            LOG("[animcheck]   Animations ahora %08X; InitSkelControls: %s arbol %08X (listas %u) num348 %u (#%u de %u); mapa: %s", Load32(r3 + 0x1FC),
                                have ? "si" : "NUNCA", si.tree, si.lists, si.num348, si.frame, g_skelInitCount.load(), mb);
                        }
                        if (fix) { if (prev) Store32(prev + 0x90, 0); else Store32(slot, 0); }
                        break;
                    }
                    prev = p; p = Load32(p + 0x90);
                }
            }
        }
    }
    __imp__sub_826A8DF8(ctx, base);
}

// Sesion 9: limite de cuadros. UGameEngine::GetMaxTickRate (sub_8255DDE8, vtable +0x134) devuelve MaxSmoothedFrameRate (30) y
// FEngineLoop::Tick (sub_829D7500) duerme 1/limite - DeltaTime. GOWJ_MAXFPS=<n> lo sustituye (0 = sin limite; por defecto 30 = consola).
// La frecuencia de VBlank emulada (GOWJ_VBLANKHZ) tambien debe subir: el juego presenta con intervalo 2.
float GowjMaxFps();
PPC_FUNC_IMPL(__imp__sub_8255DDE8);
PPC_FUNC(sub_8255DDE8)
{
    __imp__sub_8255DDE8(ctx, base);
    float cap = GowjMaxFps();
    if (cap >= 0 && ctx.f1.f64 > 0) ctx.f1.f64 = cap;   // solo cuando el juego limita (no en cargas/menus sin limite)
}
// -1 = sin cambios (30 de la consola); 0 = sin limite; n = limite de n fps
float GowjMaxFps()
{
    static const float v = [] { const char* s = getenv("GOWJ_MAXFPS"); return s ? (float)atof(s) : -1.0f; }();
    return v;
}

// Sesion 9: panoramico Hor+ (GOWJ_PANO / panoramico= 21:9, 32:9, auto). FPerspectiveMatrix (sub_826428F8: r3 = salida, f1 = medio FOV
// horizontal, f2 = ancho, f3 = alto, f4 = MinZ) de la vista principal (ancho/alto = 16/9): se amplia el FOV horizontal manteniendo el
// vertical (medio FOV' = atan(tan(medio FOV) * k), ancho' = ancho * k). El juego recorta y dibuja con esa vista; la imagen de 720p queda
// comprimida en horizontal y al presentarla a la proporcion elegida se estira (con superficies k veces mas anchas para que se vea nitida).
float PanoAspect();
PPC_FUNC_IMPL(__imp__sub_826428F8);
PPC_FUNC(sub_826428F8)
{
    static const float k = PanoAspect() / (16.0f / 9.0f);
    static const bool off = getenv("GOWJ_NOPANOFOV") != nullptr;
    if (!off && k > 1.001f && ctx.f3.f64 > 0 && std::fabs(ctx.f2.f64 / ctx.f3.f64 - 16.0 / 9.0) < 0.02)
    {
        ctx.f1.f64 = std::atan(std::tan(ctx.f1.f64) * k);
        ctx.f2.f64 *= k;
    }
    __imp__sub_826428F8(ctx, base);
}
// La vista principal (ULocalPlayer::CalcSceneView) usa sub_826427F8 = FPerspectiveMatrix(medio FOV X, medio FOV Y, MultFOVX, MultFOVY,
// ...): M00 = MultFOVX / tan, M11 = MultFOVY / tan, con MultFOVY = ancho/alto (16/9). Hor+: MultFOVX / k.
PPC_FUNC_IMPL(__imp__sub_826427F8);
PPC_FUNC(sub_826427F8)
{
    static const float k = PanoAspect() / (16.0f / 9.0f);
    static const bool off = getenv("GOWJ_NOPANOFOV") != nullptr;
    if (!off && k > 1.001f && std::fabs(ctx.f4.f64 - 16.0 / 9.0) < 0.02 && std::fabs(ctx.f3.f64 - 1.0) < 0.001) ctx.f3.f64 /= k;
    __imp__sub_826427F8(ctx, base);
}
// GoWJ sesion 10: idioma. sub_8298B3A0 = appGetLanguageExt (FString por puntero oculto en r3): registra lo que devuelve.
PPC_FUNC_IMPL(__imp__sub_8298B3A0);
PPC_FUNC(sub_8298B3A0)
{
    uint32_t out = ctx.r3.u32;
    __imp__sub_8298B3A0(ctx, base);
    static int n = 0;
    if (n++ < 3 && out)
    {
        uint32_t data = __builtin_bswap32(*(uint32_t*)(base + out)), len = __builtin_bswap32(*(uint32_t*)(base + out + 4));
        char s[16] = {}; for (uint32_t i = 0; data && i + 1 < len && i < 15; i++) s[i] = (char)base[data + i * 2 + 1];
        LOG("[idioma] el juego usa \"%s\"", s);
    }
}
PPC_FUNC_IMPL(__imp__sub_82DBBC88);
PPC_FUNC(sub_82DBBC88) { __imp__sub_82DBBC88(ctx, base); static int n = 0; if (n++ < 3) LOG("[idioma] XGetLanguage interno = %u", ctx.r3.u32); }
PPC_FUNC_IMPL(__imp__sub_82DBA8B0);
PPC_FUNC(sub_82DBA8B0) { __imp__sub_82DBA8B0(ctx, base); static int n = 0; if (n++ < 3) LOG("[idioma] XGetLocale interno = %u", ctx.r3.u32); }
PPC_FUNC_IMPL(__imp__sub_82305B60);
PPC_FUNC(sub_82305B60)
{
    __imp__sub_82305B60(ctx, base);
    static int n = 0; if (n++ >= 2) return;
    uint32_t arr = ctx.r3.u32, data = __builtin_bswap32(*(uint32_t*)(base + arr)), num = __builtin_bswap32(*(uint32_t*)(base + arr + 4));
    std::string all;
    for (uint32_t i = 0; i < num && i < 40; i++)
    {
        uint32_t sd = __builtin_bswap32(*(uint32_t*)(base + data + i * 12)), sl = __builtin_bswap32(*(uint32_t*)(base + data + i * 12 + 4));
        all += ' '; for (uint32_t k = 0; sd && k + 1 < sl && k < 12; k++) all += (char)base[sd + k * 2 + 1];
    }
    LOG("[idioma] idiomas conocidos (%u):%s", num, all.c_str());
}
// GoWJ sesion 10: XMA de XAudio. El juego lanza (kick, registro MMIO 0x650) y limpia (0x6A0) los contextos de una voz en un bucle,
// un stwbrx de un solo bit por contexto: varios seguidos del mismo grupo de 32 se pisaban antes de que el hilo del XMA los leyera
// y esos contextos no se decodificaban (sonidos y voces que faltan). Lista de la voz en r3: {u32 n, u32 flags, u32 entradas}
// con entradas de 0x60 bytes (+0x40 = contexto, +0x50 = indice u16). GOWJ_XMA_NOHOOK=1 = antes.
void XmaKickContext(uint32_t id);
void XmaClearContext(uint32_t id);
static bool XmaHookOff() { static const bool v = getenv("GOWJ_XMA_NOHOOK") != nullptr; return v; }
static uint32_t XmaListIds(uint8_t* base, uint32_t list, uint32_t* ids, bool* fresh, uint32_t max)
{
    uint32_t n = __builtin_bswap32(*(uint32_t*)(base + list)), ents = __builtin_bswap32(*(uint32_t*)(base + list + 8)), k = 0;
    for (uint32_t i = 0; i < n && k < max && ents; i++)
    {
        uint32_t e = ents + i * 0x60;
        ids[k] = __builtin_bswap16(*(uint16_t*)(base + e + 0x50));
        if (fresh) fresh[k] = *(uint32_t*)(base + e + 0x40) == 0;
        k++;
    }
    return k;
}
PPC_FUNC_IMPL(__imp__sub_82C0D7F0);
PPC_FUNC(sub_82C0D7F0)   // lanzar los contextos de una voz
{
    uint32_t list = ctx.r3.u32;
    __imp__sub_82C0D7F0(ctx, base);
    if (XmaHookOff() || !list) return;
    uint32_t ids[16]; uint32_t n = XmaListIds(base, list, ids, nullptr, 16);
    for (uint32_t i = 0; i < n; i++) XmaKickContext(ids[i]);
}
PPC_FUNC_IMPL(__imp__sub_82C0CE58);
PPC_FUNC(sub_82C0CE58)   // crear (y limpiar) los contextos de una voz
{
    uint32_t list = ctx.r3.u32, ids[16]; bool fresh[16] = {};
    uint32_t n = (!XmaHookOff() && list) ? XmaListIds(base, list, ids, fresh, 16) : 0;
    __imp__sub_82C0CE58(ctx, base);
    for (uint32_t i = 0; i < n; i++)
        if (fresh[i])
        {
            uint32_t e = __builtin_bswap32(*(uint32_t*)(base + list + 8)) + i * 0x60;
            XmaClearContext(__builtin_bswap16(*(uint16_t*)(base + e + 0x50)));   // el indice se escribe dentro de la funcion
        }
    static int nl = 0; if (n > 1 && nl++ < 5) LOG("[xma] voz con %u contextos (antes se perdian kicks/clears del mismo grupo)", n);
}
// GoWJ sesion 10 (del port de Gears 3, sesion 5): raton nativo de UE3. sub_827621F0 = InputAxis del cliente del viewport (r3 this,
// r4 controlador, r5 tecla FName de 64 bits, f1 valor, f2 tiempo del cuadro). Al llegar el eje XboxTypeS_RightX (global 0x835516B8)
// se repite la llamada con MouseX (0x83551348) / MouseY (0x83551350) y el movimiento acumulado: el motor lo trata como raton
// (aMouseX/aMouseY) sin la zona muerta, la curva ni el tope de velocidad del stick. Mismas globales que Gears 3 desplazadas -0x2D80.
// GOWJ_MOUSESTICK=1 = sistema anterior (raton -> stick derecho). GOWJ_MOUSELOG=1 registra.
extern std::atomic<int> g_mouseNativeDX, g_mouseNativeDY;
extern std::atomic<bool> g_mouseCaptured;
bool MouseNativeActive() { static const bool off = getenv("GOWJ_MOUSESTICK") != nullptr; return !off; }
PPC_FUNC_IMPL(__imp__sub_827621F0);
PPC_FUNC(sub_827621F0)
{
    uint64_t key = ctx.r5.u64;
    auto ld64 = [&](uint32_t a) { return __builtin_bswap64(*(uint64_t*)(base + a)); };
    const uint64_t rightX = ld64(0x835516B8u), mouseX = ld64(0x83551348u), mouseY = ld64(0x83551350u);
    static const bool mlog = getenv("GOWJ_MOUSELOG") != nullptr;
    static int anyLogged = 0;
    if (mlog && anyLogged < 12) { anyLogged++; LOG("[raton] InputAxis tecla %016llX valor %g (RightX %016llX)", (unsigned long long)key, ctx.f1.f64, (unsigned long long)rightX); }
    static int logged = 0;
    if (mlog && logged < 6 && key == rightX) { logged++; LOG("[raton] InputAxis RightX valor %g dt %g (MouseX %016llX MouseY %016llX)", ctx.f1.f64, ctx.f2.f64, (unsigned long long)mouseX, (unsigned long long)mouseY); }
    PPCContext saved = ctx;
    __imp__sub_827621F0(ctx, base);
    if (key == rightX && MouseNativeActive() && g_mouseCaptured)
    {
        static const float sens = getenv("GOWJ_MOUSESENS") ? (float)atof(getenv("GOWJ_MOUSESENS")) : 1.0f;
        static const bool invert = getenv("GOWJ_MOUSEINVERT") && atoi(getenv("GOWJ_MOUSEINVERT")) != 0;
        int dx = g_mouseNativeDX.exchange(0), dy = g_mouseNativeDY.exchange(0);
        if (dx) { PPCContext c = saved; c.r5.u64 = mouseX; c.f1.f64 = dx * 0.25 * sens; __imp__sub_827621F0(c, base); }
        if (dy) { PPCContext c = saved; c.r5.u64 = mouseY; c.f1.f64 = (invert ? dy : -dy) * 0.25 * sens; __imp__sub_827621F0(c, base); }
        static int nl = 0; if (mlog && (dx || dy) && nl++ < 10) LOG("[raton] inyectado dx %d dy %d", dx, dy);
    }
}

// GoWJ sesion 11 (diagnostico de cinematicas sin audio): BinkSetSoundTrack(n, ids) (sub_8309D250). UE3 elige 6 pistas por idioma
// en sub_823968E0 (INT 0-5, ..., ESN 15-17, ESM 18-20 + 3 comunes). GOWJ_BINKTRACKS=<a,b,...> fuerza la lista (prueba).
PPC_FUNC_IMPL(__imp__sub_8309D250);
PPC_FUNC(sub_8309D250)
{
    uint32_t n = ctx.r3.u32, p = ctx.r4.u32;
    static uint32_t forced[32]; static uint32_t nForced = [] { uint32_t c = 0; if (const char* s = getenv("GOWJ_BINKTRACKS")) { const char* q = s; while (*q && c < 32) { forced[c++] = (uint32_t)strtoul(q, (char**)&q, 10); while (*q == ',') q++; } } return c; }();
    {
        std::string ids; char b[16];
        for (uint32_t i = 0; i < n && i < 32 && p; i++) { snprintf(b, sizeof(b), " %u", PPC_LOAD_U32(p + i * 4)); ids += b; }
        static int nl = 0; if (nl++ < 40) LOG("[bink] BinkSetSoundTrack(%u):%s (lr %08X)%s", n, ids.c_str(), (uint32_t)ctx.lr, nForced ? " -> forzado" : "");
    }
    if (nForced)
    {   // escribe la lista forzada en la pila del llamador no: se usa un bufer propio en memoria de invitado
        static uint32_t buf = 0; if (!buf) buf = 0x83580448u - 0x100u;   // no se usa: se copian directamente abajo
        (void)buf;
        __imp__sub_8309D250(ctx, base);
        PPC_STORE_U32(0x83580448u - 8, nForced);
        for (uint32_t i = 0; i < nForced; i++) PPC_STORE_U32(0x83580448u + i * 4, forced[i]);
        return;
    }
    __imp__sub_8309D250(ctx, base);
}
