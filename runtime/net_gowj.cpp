// Red de Gears of War: Judgment (sesion 9): sockets (NetDll_*) y XNet.
//
// Sin online (por defecto): sockets locales falsos. La campana es un servidor "?listen" (UIpNetDriver::InitListen abre un socket
// UDP/VDP 0xFE); sin un socket valido LoadMap fallaba con "Error initializing network layer." (sesion 7). sendto descarta,
// recvfrom da WSAEWOULDBLOCK, el enlace Ethernet aparece desconectado.
//
// Con online (online=crear|unirse en gowj.ini -> GOWJ_NET=crear|unirse): enlace de sistema P2P sobre UDP real. Cada socket del juego
// es un socket UDP del PC (VDP 0xFE incluido: los dos extremos son este port, el contenido pasa tal cual). La direccion XNet de la
// consola (XNADDR.ina) es la IP real del PC (o la de la VPN; GOWJ_NET_IP la fija), asi que XNetXnAddrToInAddr es la identidad.
// Puerto del juego P -> puerto del PC P + (base - 1000) (base = online_puerto, 3074 por defecto: el VDP 1000 del juego va al 3074).
// Las difusiones LAN (255.255.255.255, x.x.x.255: el "LAN beacon" de UE3) se envian a la direccion del anfitrion (online_direccion)
// y a todos los pares de los que se ha recibido algo. Prueba en un solo PC: GOWJ_NET_IP=127.0.0.1 en uno y 127.0.0.2 en el otro.
#include "runtime.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include <random>
#include <algorithm>
void TexDirtyTouch(uint32_t addr, uint32_t len);   // texdirty.cpp (sesion 10)
#pragma comment(lib, "ws2_32.lib")

namespace
{
struct NetCfg
{
    bool online = false; bool host = false;
    uint32_t localIp = 0x7F000001;     // orden de host
    uint32_t bindIp = 0;               // 0 = INADDR_ANY
    int portOffset = 3074 - 1000;
    std::vector<uint32_t> peers;       // IPs (orden de host) a las que van las difusiones
};
uint32_t ParseIp(const char* s)
{
    unsigned a, b, c, d;
    if (!s || sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a > 255 || b > 255 || c > 255 || d > 255) return 0;
    return (a << 24) | (b << 16) | (c << 8) | d;
}
std::string IpStr(uint32_t ip) { char b[32]; snprintf(b, sizeof(b), "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255); return b; }
// La IPv4 del PC: la primera que no sea de bucle invertido (las de VPN tipo Radmin 26.x o ZeroTier tambien valen)
uint32_t DetectLocalIp()
{
    char name[256] = {};
    if (gethostname(name, sizeof(name)) != 0) return 0x7F000001;
    addrinfo hints{}, *res = nullptr; hints.ai_family = AF_INET;
    uint32_t best = 0;
    if (getaddrinfo(name, nullptr, &hints, &res) == 0)
    {
        for (addrinfo* p = res; p; p = p->ai_next)
        {
            uint32_t ip = ntohl(((sockaddr_in*)p->ai_addr)->sin_addr.s_addr);
            if ((ip >> 24) == 127 || (ip >> 16) == 0xA9FE) continue;   // bucle invertido, APIPA
            if (!best) best = ip;
        }
        freeaddrinfo(res);
    }
    return best ? best : 0x7F000001;
}
NetCfg& Cfg()
{
    static NetCfg c = [] {
        NetCfg n;
        const char* mode = getenv("GOWJ_NET");
        n.online = mode && (!strcmp(mode, "crear") || !strcmp(mode, "unirse"));
        n.host = mode && !strcmp(mode, "crear");
        if (!n.online) return n;
        WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
        if (const char* p = getenv("GOWJ_NET_PORT")) { int v = atoi(p); if (v > 1024 && v < 60000) n.portOffset = v - 1000; }
        n.localIp = ParseIp(getenv("GOWJ_NET_IP"));
        if (!n.localIp) n.localIp = DetectLocalIp();
        if (getenv("GOWJ_NET_IP")) n.bindIp = n.localIp;   // prueba local con varias IPs de bucle invertido
        if (uint32_t peer = ParseIp(getenv("GOWJ_NET_PEER"))) n.peers.push_back(peer);
        LOG("[net] enlace de sistema P2P: %s, IP %s, puerto base %d%s%s", n.host ? "anfitrion" : "invitado", IpStr(n.localIp).c_str(), n.portOffset + 1000,
            n.peers.empty() ? "" : ", anfitrion ", n.peers.empty() ? "" : IpStr(n.peers[0]).c_str());
        return n;
    }();
    return c;
}

thread_local uint32_t t_wsaError = 0;
std::mutex g_sockMutex;
struct GuestSock { SOCKET s = INVALID_SOCKET; uint16_t port = 0; bool udp = true; bool vdp = false; };
std::map<uint32_t, GuestSock> g_socks;
std::atomic<uint32_t> g_nextSock{ 0x100 };
std::set<uint32_t> g_learnedPeers;

bool IsBroadcast(uint32_t ip) { return ip == 0xFFFFFFFFu || (ip & 0xFF) == 0xFF; }
GuestSock* Find(uint32_t s) { auto it = g_socks.find(s); return it == g_socks.end() ? nullptr : &it->second; }
void SetErr() { t_wsaError = (uint32_t)WSAGetLastError(); }
// sockaddr_in del invitado (big-endian) -> IP y puerto (orden de host)
void ReadAddr(uint32_t a, uint32_t& ip, uint16_t& port) { port = (uint16_t)((g_base[a + 2] << 8) | g_base[a + 3]); ip = Load32(a + 4); }
void WriteAddr(uint32_t a, uint32_t ip, uint16_t port)
{
    memset(g_base + a, 0, 16); g_base[a + 1] = 2; g_base[a + 2] = (uint8_t)(port >> 8); g_base[a + 3] = (uint8_t)port; Store32(a + 4, ip);
}
sockaddr_in HostAddr(uint32_t ip, uint16_t guestPort)
{
    sockaddr_in sa{}; sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(ip); sa.sin_port = htons((uint16_t)(guestPort + Cfg().portOffset)); return sa;
}
} // namespace

// ---------------- sockets ----------------
// socket(caller, af, type, protocol)
GUEST_IMPL(NetDll_socket)
{
    NetCfg& c = Cfg();
    uint32_t id = g_nextSock++;
    GuestSock gs; gs.udp = ctx.r5.u32 != 1;   // SOCK_STREAM = 1
    gs.vdp = ctx.r6.u32 == 254;              // IPPROTO_VDP: conexiones del juego
    if (c.online)
    {
        gs.s = ::socket(AF_INET, gs.udp ? SOCK_DGRAM : SOCK_STREAM, gs.udp ? IPPROTO_UDP : IPPROTO_TCP);
        if (gs.s == INVALID_SOCKET) { SetErr(); ctx.r3.u64 = 0xFFFFFFFF; return; }
        u_long nb = 1; ioctlsocket(gs.s, FIONBIO, &nb);
        BOOL on = TRUE; setsockopt(gs.s, SOL_SOCKET, SO_BROADCAST, (const char*)&on, sizeof(on));
    }
    { std::lock_guard<std::mutex> l(g_sockMutex); g_socks[id] = gs; }
    LOG("[net] socket(af %u, tipo %u, proto %u) -> %X%s", ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, id, c.online ? " (UDP real)" : "");
    ctx.r3.u64 = id;
}
// bind(caller, s, sockaddr*, len)
GUEST_IMPL(NetDll_bind)
{
    uint32_t ip; uint16_t port; ReadAddr(ctx.r5.u32, ip, port);
    if (!port) port = 1000;
    std::lock_guard<std::mutex> l(g_sockMutex);
    GuestSock* gs = Find(ctx.r4.u32);
    if (!gs) { t_wsaError = WSAENOTSOCK; ctx.r3.u64 = 0xFFFFFFFF; return; }
    gs->port = port;
    if (gs->s != INVALID_SOCKET)
    {
        // Sesion 10: el invitado vuelve a enlazar el mismo puerto del juego al viajar (grupo -> lobby -> partida). Si usa la misma
        // IP:puerto, al nuevo driver del anfitrion le llegan antes paquetes rezagados de la conexion vieja, UE3 crea la conexion
        // con ese estado y descarta el "hola" nuevo como paquete antiguo (el invitado se quedaba cargando). Cada reenlace del mismo
        // puerto usa otro puerto del PC (+100); el anfitrion lo ve como otro puerto de origen. El anfitrion conserva el suyo (los
        // invitados le envian al 1000 del juego).
        uint16_t effective = port;
        if (!Cfg().host && gs->vdp) { static std::map<uint16_t, int> gens; int gen = gens[port]++; if (gen > 0 && gen < 60) effective = (uint16_t)(port + 100 * gen); }
        sockaddr_in sa = HostAddr(Cfg().bindIp, effective);
        if (::bind(gs->s, (sockaddr*)&sa, sizeof(sa)) != 0) { SetErr(); LOG("[net] bind(%X, puerto %u -> %u) fallo (%lu)", ctx.r4.u32, port, ntohs(sa.sin_port), (unsigned long)t_wsaError); ctx.r3.u64 = 0xFFFFFFFF; return; }
        LOG("[net] bind(%X, puerto del juego %u -> UDP %s:%u)", ctx.r4.u32, port, IpStr(Cfg().bindIp).c_str(), ntohs(sa.sin_port));
    }
    else LOG("[net] bind(%X, puerto %u)", ctx.r4.u32, port);
    ctx.r3.u64 = 0;
}
GUEST_IMPL(NetDll_closesocket)
{
    std::lock_guard<std::mutex> l(g_sockMutex);
    if (GuestSock* gs = Find(ctx.r4.u32)) { if (gs->s != INVALID_SOCKET) ::closesocket(gs->s); g_socks.erase(ctx.r4.u32); }
    ctx.r3.u64 = 0;
}
GUEST_IMPL(NetDll_ioctlsocket) { ctx.r3.u64 = 0; }   // los sockets reales ya son no bloqueantes
GUEST_IMPL(NetDll_setsockopt) { ctx.r3.u64 = 0; }
GUEST_IMPL(NetDll_getsockopt) { ctx.r3.u64 = 0; }
GUEST_IMPL(NetDll_listen) { ctx.r3.u64 = 0; }
GUEST_IMPL(NetDll_accept) { t_wsaError = WSAEWOULDBLOCK; ctx.r3.u64 = 0xFFFFFFFF; }
GUEST_IMPL(NetDll_connect) { t_wsaError = WSAECONNREFUSED; ctx.r3.u64 = 0xFFFFFFFF; }
// getsockname(caller, s, sockaddr*, int* len)
GUEST_IMPL(NetDll_getsockname)
{
    uint16_t port = 1000;
    { std::lock_guard<std::mutex> l(g_sockMutex); if (GuestSock* gs = Find(ctx.r4.u32)) if (gs->port) port = gs->port; }
    if (ctx.r5.u32) WriteAddr(ctx.r5.u32, Cfg().localIp, port);
    if (ctx.r6.u32) Store32(ctx.r6.u32, 16);
    ctx.r3.u64 = 0;
}
// sendto(caller, s, buf, len, flags, to, tolen)
GUEST_IMPL(NetDll_sendto)
{
    NetCfg& c = Cfg();
    uint32_t len = ctx.r6.u32;
    SOCKET hs = INVALID_SOCKET;
    { std::lock_guard<std::mutex> l(g_sockMutex); if (GuestSock* gs = Find(ctx.r4.u32)) hs = gs->s; }
    if (!c.online || hs == INVALID_SOCKET || !ctx.r8.u32) { ctx.r3.u64 = len; return; }
    uint32_t ip; uint16_t port; ReadAddr(ctx.r8.u32, ip, port);
    const char* data = (const char*)(g_base + ctx.r5.u32);
    std::vector<uint32_t> dests;
    if (IsBroadcast(ip) || ip == 0)   // 0.0.0.0: direccion de sesion sin rellenar -> a los pares conocidos
    {
        dests = c.peers;
        std::lock_guard<std::mutex> l(g_sockMutex);
        for (uint32_t p : g_learnedPeers) if (std::find(dests.begin(), dests.end(), p) == dests.end()) dests.push_back(p);
    }
    else dests.push_back(ip);
    for (uint32_t d : dests)
    {
        if (d == c.localIp) continue;
        sockaddr_in sa = HostAddr(d, port);
        if (::sendto(hs, data, (int)len, 0, (sockaddr*)&sa, sizeof(sa)) == SOCKET_ERROR) SetErr();
    }
    {   // registro: los primeros envios de cada (socket, destino)
        static std::mutex lm; static std::map<uint64_t, int> seen;
        std::lock_guard<std::mutex> l(lm);
        int& k = seen[(uint64_t(ctx.r4.u32) << 48) ^ (uint64_t(ip) << 16) ^ port];
        if (k++ < (getenv("GOWJ_NETHEX") ? 12 : 3) || (k % 500) == 0)
        {
            char hex[3 * 48 + 1] = {}; int c = 0; for (uint32_t i = 0; i < len && i < 48; i++) c += snprintf(hex + c, sizeof(hex) - c, "%02X ", (uint8_t)data[i]);
            LOG("[net] sendto #%d socket %X (%u bytes) a %s:%u%s (%zu destinos) %s", k, ctx.r4.u32, len, IpStr(ip).c_str(), port, IsBroadcast(ip) || !ip ? " (difusion)" : "", dests.size(), getenv("GOWJ_NETHEX") ? hex : "");
        }
    }
    ctx.r3.u64 = len;
}
GUEST_IMPL(NetDll_send) { ctx.r3.u64 = ctx.r6.u32; }
// recvfrom(caller, s, buf, len, flags, from, fromlen*)
GUEST_IMPL(NetDll_recvfrom)
{
    SOCKET hs = INVALID_SOCKET;
    { std::lock_guard<std::mutex> l(g_sockMutex); if (GuestSock* gs = Find(ctx.r4.u32)) hs = gs->s; }
    if (hs == INVALID_SOCKET) { t_wsaError = WSAEWOULDBLOCK; ctx.r3.u64 = 0xFFFFFFFF; return; }
    sockaddr_in from{}; int fl = sizeof(from);
    TexDirtyTouch(ctx.r5.u32, ctx.r6.u32);   // sesion 10 (texdirty.cpp)
    int r = ::recvfrom(hs, (char*)(g_base + ctx.r5.u32), (int)ctx.r6.u32, 0, (sockaddr*)&from, &fl);
    if (r == SOCKET_ERROR)
    {
        int e = WSAGetLastError();
        t_wsaError = (e == WSAECONNRESET) ? WSAEWOULDBLOCK : (uint32_t)e;   // ICMP "puerto inalcanzable" de envios anteriores: no es un error del juego
        ctx.r3.u64 = 0xFFFFFFFF; return;
    }
    uint32_t ip = ntohl(from.sin_addr.s_addr); uint16_t port = (uint16_t)(ntohs(from.sin_port) - Cfg().portOffset);
    {
        std::lock_guard<std::mutex> l(g_sockMutex);
        if (ip != Cfg().localIp && g_learnedPeers.insert(ip).second) LOG("[net] nuevo par: %s", IpStr(ip).c_str());
    }
    if (ctx.r8.u32) WriteAddr(ctx.r8.u32, ip, port);
    if (ctx.r9.u32) Store32(ctx.r9.u32, 16);
    {
        static std::mutex lm; static std::map<uint64_t, int> seen;
        std::lock_guard<std::mutex> l(lm);
        int& k = seen[(uint64_t(ctx.r4.u32) << 48) ^ (uint64_t(ip) << 16) ^ port];
        if (k++ < (getenv("GOWJ_NETHEX") ? 12 : 3) || (k % 500) == 0)
        {
            const uint8_t* d = g_base + ctx.r5.u32; char hex[3 * 48 + 1] = {}; int c = 0; for (int i = 0; i < r && i < 48; i++) c += snprintf(hex + c, sizeof(hex) - c, "%02X ", d[i]);
            LOG("[net] recvfrom #%d socket %X (%d bytes) de %s:%u %s", k, ctx.r4.u32, r, IpStr(ip).c_str(), port, getenv("GOWJ_NETHEX") ? hex : "");
        }
    }
    ctx.r3.u64 = (uint32_t)r;
}
GUEST_IMPL(NetDll_recv) { t_wsaError = WSAEWOULDBLOCK; ctx.r3.u64 = 0xFFFFFFFF; }
// select(caller, nfds, read, write, except, timeval*): fd_set del invitado = cuenta + 64 sockets (big-endian)
GUEST_IMPL(NetDll_select)
{
    uint32_t rd = ctx.r5.u32, wr = ctx.r6.u32, ex = ctx.r7.u32, tv = ctx.r8.u32;
    int ready = 0;
    if (rd)
    {
        uint32_t n = std::min<uint32_t>(Load32(rd), 64), out = 0;
        std::vector<std::pair<uint32_t, SOCKET>> fds;
        { std::lock_guard<std::mutex> l(g_sockMutex); for (uint32_t i = 0; i < n; i++) { uint32_t s = Load32(rd + 4 + i * 4); GuestSock* gs = Find(s); if (gs && gs->s != INVALID_SOCKET) fds.push_back({ s, gs->s }); } }
        if (!fds.empty())
        {
            fd_set hs; FD_ZERO(&hs); for (auto& f : fds) FD_SET(f.second, &hs);
            timeval t{ 0, 0 }; if (tv) { t.tv_sec = (long)Load32(tv); t.tv_usec = (long)Load32(tv + 4); if (t.tv_sec > 0) t.tv_sec = 0, t.tv_usec = 100000; }
            if (::select(0, &hs, nullptr, nullptr, &t) > 0) for (auto& f : fds) if (FD_ISSET(f.second, &hs)) Store32(rd + 4 + (out++) * 4, f.first);
        }
        Store32(rd, out); ready += (int)out;
    }
    if (wr) ready += (int)Load32(wr);
    if (ex) Store32(ex, 0);
    ctx.r3.u64 = (uint32_t)ready;
}
GUEST_IMPL(NetDll_WSAGetLastError) { ctx.r3.u64 = t_wsaError; }
GUEST_IMPL(NetDll_WSASetLastError) { t_wsaError = ctx.r3.u32; ctx.r3.u64 = 0; }
// inet_addr(const char*)
GUEST_IMPL(NetDll_inet_addr) { uint32_t ip = ParseIp((const char*)(g_base + ctx.r3.u32)); ctx.r3.u64 = ip ? ip : 0xFFFFFFFF; }

// ---------------- XNet ----------------
// XNetGetTitleXnAddr(caller, XNADDR*): XNADDR = ina(4) inaOnline(4) wPortOnline(2) abEnet[6] abOnline[20]. Con online, ina = IP del PC
// y abEnet una MAC derivada de ella (distinta por PC); XNET_GET_XNADDR_STATIC | ETHERNET.
GUEST_IMPL(NetDll_XNetGetTitleXnAddr)
{
    uint32_t a = ctx.r4.u32, ip = Cfg().localIp;
    if (a)
    {
        memset(g_base + a, 0, 36);
        Store32(a, ip);
        uint8_t mac[6] = { 0x00, 0x22, (uint8_t)(ip >> 24), (uint8_t)(ip >> 16), (uint8_t)(ip >> 8), (uint8_t)ip };
        memcpy(g_base + a + 10, mac, 6);
    }
    ctx.r3.u64 = 0x04 | (Cfg().online ? 0x01 : 0);   // STATIC (| ETHERNET)
}
// XNetXnAddrToInAddr(caller, XNADDR*, XNKID*, IN_ADDR*): la "IP segura" es la propia ina
GUEST_IMPL(NetDll_XNetXnAddrToInAddr) { if (ctx.r6.u32 && ctx.r4.u32) Store32(ctx.r6.u32, Load32(ctx.r4.u32)); ctx.r3.u64 = 0; }
// XNetInAddrToXnAddr(caller, ina, XNADDR*, XNKID*)
GUEST_IMPL(NetDll_XNetInAddrToXnAddr)
{
    uint32_t ip = ctx.r4.u32, a = ctx.r5.u32;
    if (a)
    {
        memset(g_base + a, 0, 36); Store32(a, ip);
        uint8_t mac[6] = { 0x00, 0x22, (uint8_t)(ip >> 24), (uint8_t)(ip >> 16), (uint8_t)(ip >> 8), (uint8_t)ip };
        memcpy(g_base + a + 10, mac, 6);
    }
    if (ctx.r6.u32) memset(g_base + ctx.r6.u32, 0, 8);
    ctx.r3.u64 = 0;
}
// XNetRandom(caller, buffer, length): bytes aleatorios (claves de sesion y nonces)
GUEST_IMPL(NetDll_XNetRandom)
{
    static std::mt19937_64 rng{ std::random_device{}() ^ (uint64_t)GetTickCount64() };
    for (uint32_t i = 0; i < ctx.r5.u32; i++) g_base[ctx.r4.u32 + i] = (uint8_t)rng();
    ctx.r3.u64 = 0;
}
GUEST_IMPL(NetDll_XNetConnect) { ctx.r3.u64 = 0; }
GUEST_IMPL(NetDll_XNetRegisterKey) { ctx.r3.u64 = 0; }
GUEST_IMPL(NetDll_XNetUnregisterKey) { ctx.r3.u64 = 0; }
GUEST_IMPL(NetDll_XNetServerToInAddr) { ctx.r3.u64 = 0x80004005; }
GUEST_IMPL(NetDll_XNetQosListen) { ctx.r3.u64 = 0; }
GUEST_IMPL(NetDll_XNetQosRelease) { ctx.r3.u64 = 0; }
GUEST_IMPL(NetDll_XNetQosGetListenStats) { ctx.r3.u64 = 0; }
GUEST_IMPL(NetDll_XNetQosLookup) { ctx.r3.u64 = 0x80004005; }
// XNetGetEthernetLinkStatus(caller): con online, enlace activo a 100 Mb en duplex (ACTIVE | 100MBPS | FULL_DUPLEX)
GUEST_IMPL(NetDll_XNetGetEthernetLinkStatus) { ctx.r3.u64 = Cfg().online ? 0x0B : 0; }

// XNADDR de esta consola (para XSESSION_INFO del anfitrion, xam.cpp)
void XnAddrFill(uint32_t a)
{
    uint32_t ip = Cfg().localIp;
    memset(g_base + a, 0, 36); Store32(a, ip);
    uint8_t mac[6] = { 0x00, 0x22, (uint8_t)(ip >> 24), (uint8_t)(ip >> 16), (uint8_t)(ip >> 8), (uint8_t)ip };
    memcpy(g_base + a + 10, mac, 6);
}
