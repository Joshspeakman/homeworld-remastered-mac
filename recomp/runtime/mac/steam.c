/* Steamworks for the guest: an offline Steam that owns the game (the user's own Steam copy). Multiplayer through
 * Steam lobbies comes later.
 *
 * The flat functions (SteamAPI_Init, SteamUser(), ...) are imports; each accessor returns a C++ interface object
 * whose vtable slots are thunks named "<Accessor>#<slot>". Methods are cdecl with `this` first (i386 GCC ABI); a
 * slot the game calls that is not served here stops with its name, so the next one to write is always known. */
#include "mac.h"

#define STEAM_ID 76561197960287930ull                  /* an individual account in the public universe */
#define HWRM_APP 244160u

static uint32_t iface(const char *name)
{
    char key[96]; snprintf(key, sizeof key, "steam %s", name);
    uint32_t obj = g_data(key, 16);
    if (!rt_r32(G_MEM, obj)) {
        snprintf(key, sizeof key, "steam %s vtable", name);
        uint32_t vt = g_data(key, 4 * 160);
        for (uint32_t k = 0; k < 160; k++) { char t[96]; snprintf(t, sizeof t, "%s#%u", name, k); rt_w32(G_MEM, vt + 4 * k, g_thunk(t)); }
        rt_w32(G_MEM, obj, vt);
    }
    return obj;
}
static uint32_t gstr(const char *s) { static struct { const char *s; uint32_t g; } c[32]; static int n;
    for (int k = 0; k < n; k++) if (c[k].s == s) return c[k].g;
    uint32_t g = g_strdup(s); if (n < 32) { c[n].s = s; c[n].g = g; n++; } return g; }

IMP(_SteamAPI_Init) { hw_log("Steam: offline (the game is the user's own Steam copy)"); RET(1); }
IMP(_SteamAPI_Shutdown) { RETV(); }
/* Callbacks, as Steam delivers them: the game registers CCallback objects (vtable slot 0 = Run(void *param),
 * m_iCallback at +8); the answers to its requests are queued and handed to every object registered for that id in
 * SteamAPI_RunCallbacks. Offline, the stats requests succeed with no stats; the game's quit path waits for them (up to
 * 10 s per wait) before exiting. */
static uint32_t cbs[64][2]; static int ncbs;                 /* (object, callback id) */
static struct { int id; uint8_t data[64]; int size; } pending[32]; static int npending;
static void post(int id, const void *data, int size) { if (npending < 32) { pending[npending].id = id; memcpy(pending[npending].data, data, (size_t)size); pending[npending].size = size; npending++; } }
IMP(_SteamAPI_RunCallbacks)
{
    int n = npending; npending = 0;
    for (int k = 0; k < n; k++) {
        uint32_t param = g_malloc(64); memcpy(GP(param), pending[k].data, (size_t)pending[k].size);
        for (int j = 0; j < ncbs; j++) if (cbs[j][1] == (uint32_t)pending[k].id) {
            uint32_t obj = cbs[j][0], vt = rt_r32(G_MEM, obj), run = rt_r32(G_MEM, vt);
            uint32_t args[2] = {obj, param}; g_call(c, run, 2, args);
        }
        g_free(param);
    }
    RETV();
}
IMP(_SteamAPI_RegisterCallback)
{
    if (ncbs < 64) { cbs[ncbs][0] = ARG(0); cbs[ncbs][1] = ARG(1); ncbs++; }
    if (getenv("HW_LOG_STEAM")) hw_log("Steam: callback %u registered", ARG(1));
    RETV();
}
IMP(_SteamAPI_UnregisterCallback)
{
    for (int k = 0; k < ncbs; k++) if (cbs[k][0] == ARG(0)) { memmove(cbs[k], cbs[k + 1], sizeof cbs[0] * (size_t)(ncbs - k - 1)); ncbs--; k--; }
    RETV();
}
IMP(_SteamAPI_RegisterCallResult) { RETV(); }
IMP(_SteamAPI_UnregisterCallResult) { RETV(); }
IMP(_SteamUser) { RET(iface("SteamUser")); }
IMP(_SteamFriends) { RET(iface("SteamFriends")); }
IMP(_SteamUtils) { RET(iface("SteamUtils")); }
IMP(_SteamApps) { RET(iface("SteamApps")); }
IMP(_SteamUserStats) { RET(iface("SteamUserStats")); }
IMP(_SteamMatchmaking) { RET(iface("SteamMatchmaking")); }
IMP(_SteamMatchmakingServers) { RET(iface("SteamMatchmakingServers")); }
IMP(_SteamNetworking) { RET(iface("SteamNetworking")); }
IMP(_SteamRemoteStorage) { RET(iface("SteamRemoteStorage")); }
IMP(_SteamScreenshots) { RET(iface("SteamScreenshots")); }

/* ISteamUser */
IMP_AS(user_hsteamuser, "SteamUser#0") { RET(1); }
IMP_AS(user_loggedon, "SteamUser#1") { RET(1); }
IMP_AS(user_steamid, "SteamUser#2") { RET64(STEAM_ID); }   /* CSteamID: 8 bytes, returned in edx:eax (no hidden pointer) */
/* ISteamApps (ISteamApps006/007) */
IMP_AS(apps_subscribed, "SteamApps#0") { RET(1); }
IMP_AS(apps_lowviolence, "SteamApps#1") { RET(0); }
IMP_AS(apps_cybercafe, "SteamApps#2") { RET(0); }
IMP_AS(apps_vacbanned, "SteamApps#3") { RET(0); }
IMP_AS(apps_language, "SteamApps#4") { RET(gstr("english")); }
IMP_AS(apps_languages, "SteamApps#5") { RET(gstr("english")); }
IMP_AS(apps_subscribedapp, "SteamApps#6") { RET(1); }
IMP_AS(apps_dlcinstalled, "SteamApps#7") { RET(1); }
/* ISteamUtils */
IMP_AS(utils_appid, "SteamUtils#9") { RET(HWRM_APP); }
/* ISteamFriends */
IMP_AS(friends_personaname, "SteamFriends#0") { const char *u = getenv("USER"); RET(gstr(u ? u : "Commander")); }
/* ISteamUserStats: RequestCurrentStats() succeeds (no stats arrive offline) */
/* UserStatsReceived_t (1101) / UserStatsStored_t (1102): { uint64 gameID; EResult result (1 = OK); [CSteamID user] }, pack 4 */
static void stats_answer(int id)
{
    uint8_t d[20] = {0}; uint64_t game = HWRM_APP, user = STEAM_ID; uint32_t ok = 1;
    memcpy(d, &game, 8); memcpy(d + 8, &ok, 4); memcpy(d + 12, &user, 8);
    post(id, d, id == 1101 ? 20 : 12);
}
IMP_AS(stats_request, "SteamUserStats#0") { stats_answer(1101); RET(1); }
IMP_AS(stats_store, "SteamUserStats#10") { stats_answer(1102); RET(1); }
