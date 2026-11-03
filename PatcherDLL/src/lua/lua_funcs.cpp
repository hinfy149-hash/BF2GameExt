#include "pch.h"
#include "util/content_census.hpp"
#include "util/ini_state.hpp"
#include "lua_funcs.hpp"
#include "lua_hooks.hpp"
#include "core/resolve.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/lvl_read.hpp"
#include "entity/flyer_carrier_fixes.hpp"
#include "entity/instance_props.hpp"
#include "weapon/barrel_fire_origin.hpp"   // engine_ray_hit (world raycast, per-build safe)
#include "core/pbl_hash.hpp"
#include "game/Battlefront2/Source/Character.h"
#include "game/Battlefront2/Source/Weapon.h"
#include <detours.h>
#include <wininet.h>
#include <math.h>   // sqrtf (ballistic arc)
#pragma comment(lib, "wininet.lib")

// ReadTextFile(path) - reads any file on disk, returns its contents as a string.
// Accepts absolute or relative paths. Returns nil if the file cannot be opened.
// Example: local cfg = ReadTextFile("C:\\Users\\me\\Desktop\\config.txt")
//
// WARNING: This allows reading arbitrary files from the filesystem with no
// sandboxing or path restrictions. Disabled until a safe path policy is in place.
#if 0
static int lua_ReadTextFile(lua_State* L)
{
   const char* path = g_lua.tolstring(L, 1, nullptr);
   if (!path) { g_lua.pushnil(L); return 1; }

   FILE* f = nullptr;
   if (fopen_s(&f, path, "rb") != 0 || !f) { g_lua.pushnil(L); return 1; }

   fseek(f, 0, SEEK_END);
   long size = ftell(f);
   rewind(f);

   char* buf = (char*)malloc(size + 1);
   if (!buf) { fclose(f); g_lua.pushnil(L); return 1; }

   size_t read = fread(buf, 1, size, f);
   fclose(f);
   buf[read] = '\0';

   g_lua.pushlstring(L, buf, read);
   free(buf);
   return 1;
}
#endif

// Opens the WinINet session every Http* call uses. The default timeouts let a
// server that never answers hold the call far longer, and the synchronous
// calls hold the game thread with it.
static constexpr DWORD kHttpTimeoutMs = 5000;

static HINTERNET http_open_session()
{
   HINTERNET hNet = InternetOpenA("BF2GameExt", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
   if (!hNet) return nullptr;
   DWORD ms = kHttpTimeoutMs;
   InternetSetOptionA(hNet, INTERNET_OPTION_CONNECT_TIMEOUT, &ms, sizeof(ms));
   InternetSetOptionA(hNet, INTERNET_OPTION_SEND_TIMEOUT,    &ms, sizeof(ms));
   InternetSetOptionA(hNet, INTERNET_OPTION_RECEIVE_TIMEOUT, &ms, sizeof(ms));
   return hNet;
}

// HttpGet(url) - performs a synchronous HTTP GET, returns response body as a string.
// Returns nil on failure. Supports http and https.
// Example: local body = HttpGet("http://example.com/data.txt")
static int lua_HttpGet(lua_State* L)
{
   const char* url = g_lua.tolstring(L, 1, nullptr);
   if (!url) { g_lua.pushnil(L); return 1; }

   HINTERNET hNet = http_open_session();
   if (!hNet) { g_lua.pushnil(L); return 1; }

   HINTERNET hUrl = InternetOpenUrlA(hNet, url, nullptr, 0,
                                     INTERNET_FLAG_RELOAD |
                                     INTERNET_FLAG_IGNORE_CERT_CN_INVALID |
                                     INTERNET_FLAG_IGNORE_CERT_DATE_INVALID, 0);
   if (!hUrl) { InternetCloseHandle(hNet); g_lua.pushnil(L); return 1; }

   // Read response into a growable buffer
   char chunk[4096];
   DWORD bytes_read = 0;
   size_t total = 0;
   size_t capacity = 65536;
   char* buf = (char*)malloc(capacity);
   if (!buf) { InternetCloseHandle(hUrl); InternetCloseHandle(hNet); g_lua.pushnil(L); return 1; }

   while (InternetReadFile(hUrl, chunk, sizeof(chunk), &bytes_read) && bytes_read > 0) {
      if (total + bytes_read > capacity) {
         capacity *= 2;
         char* newbuf = (char*)realloc(buf, capacity);
         if (!newbuf) { free(buf); InternetCloseHandle(hUrl); InternetCloseHandle(hNet); g_lua.pushnil(L); return 1; }
         buf = newbuf;
      }
      memcpy(buf + total, chunk, bytes_read);
      total += bytes_read;
   }

   InternetCloseHandle(hUrl);
   InternetCloseHandle(hNet);

   g_lua.pushlstring(L, buf, total);
   free(buf);
   return 1;
}

// HttpPut(url, body) - performs a synchronous HTTP PUT, returns response body as a string.
// Returns nil on failure. Supports http and https.
// Uses the lower-level WinINet API (InternetConnect + HttpOpenRequest) because
// InternetOpenUrlA does not support specifying an HTTP method.
// Example: local resp = HttpPut("http://example.com/api/data", "{\"key\":\"value\"}")
static int lua_HttpPut(lua_State* L)
{
   const char* url = g_lua.tolstring(L, 1, nullptr);
   if (!url) { g_lua.pushnil(L); return 1; }

   size_t bodyLen = 0;
   const char* body = g_lua.tolstring(L, 2, &bodyLen);
   // body may be nil (PUT with empty body is valid)

   // Parse URL into host, path, port, scheme
   URL_COMPONENTSA uc = {};
   uc.dwStructSize = sizeof(uc);
   char hostBuf[256]  = {};
   char pathBuf[2048] = {};
   uc.lpszHostName     = hostBuf;
   uc.dwHostNameLength = sizeof(hostBuf);
   uc.lpszUrlPath      = pathBuf;
   uc.dwUrlPathLength  = sizeof(pathBuf);
   if (!InternetCrackUrlA(url, 0, 0, &uc)) { g_lua.pushnil(L); return 1; }

   const BOOL   isHttps = (uc.nScheme == INTERNET_SCHEME_HTTPS);
   const DWORD  flags   = INTERNET_FLAG_RELOAD |
                          (isHttps ? INTERNET_FLAG_SECURE |
                                     INTERNET_FLAG_IGNORE_CERT_CN_INVALID |
                                     INTERNET_FLAG_IGNORE_CERT_DATE_INVALID : 0);
   const INTERNET_PORT port = uc.nPort ? uc.nPort
                            : (isHttps ? INTERNET_DEFAULT_HTTPS_PORT
                                       : INTERNET_DEFAULT_HTTP_PORT);

   HINTERNET hNet = http_open_session();
   if (!hNet) { g_lua.pushnil(L); return 1; }

   HINTERNET hConn = InternetConnectA(hNet, hostBuf, port,
                                      nullptr, nullptr, INTERNET_SERVICE_HTTP, 0, 0);
   if (!hConn) { InternetCloseHandle(hNet); g_lua.pushnil(L); return 1; }

   const char* path = pathBuf[0] ? pathBuf : "/";
   HINTERNET hReq = HttpOpenRequestA(hConn, "PUT", path,
                                     nullptr, nullptr, nullptr, flags, 0);
   if (!hReq) { InternetCloseHandle(hConn); InternetCloseHandle(hNet); g_lua.pushnil(L); return 1; }

   if (!HttpSendRequestA(hReq, nullptr, 0,
                         (LPVOID)body, body ? (DWORD)bodyLen : 0)) {
      InternetCloseHandle(hReq); InternetCloseHandle(hConn); InternetCloseHandle(hNet);
      g_lua.pushnil(L); return 1;
   }

   // Read response into a growable buffer
   char chunk[4096];
   DWORD bytes_read = 0;
   size_t total = 0;
   size_t capacity = 65536;
   char* buf = (char*)malloc(capacity);
   if (!buf) { InternetCloseHandle(hReq); InternetCloseHandle(hConn); InternetCloseHandle(hNet); g_lua.pushnil(L); return 1; }

   while (InternetReadFile(hReq, chunk, sizeof(chunk), &bytes_read) && bytes_read > 0) {
      if (total + bytes_read > capacity) {
         capacity *= 2;
         char* newbuf = (char*)realloc(buf, capacity);
         if (!newbuf) { free(buf); InternetCloseHandle(hReq); InternetCloseHandle(hConn); InternetCloseHandle(hNet); g_lua.pushnil(L); return 1; }
         buf = newbuf;
      }
      memcpy(buf + total, chunk, bytes_read);
      total += bytes_read;
   }

   InternetCloseHandle(hReq);
   InternetCloseHandle(hConn);
   InternetCloseHandle(hNet);

   g_lua.pushlstring(L, buf, total);
   free(buf);
   return 1;
}

// HttpPost(url, body) - performs a synchronous HTTP POST with Content-Type: application/json.
// Returns response body as a string, or nil on failure. Supports http and https.
// Example: local resp = HttpPost("http://example.com/api", "{\"key\":\"value\"}")
static int lua_HttpPost(lua_State* L)
{
   const char* url = g_lua.tolstring(L, 1, nullptr);
   if (!url) { g_lua.pushnil(L); return 1; }

   size_t bodyLen = 0;
   const char* body = g_lua.tolstring(L, 2, &bodyLen);

   URL_COMPONENTSA uc = {};
   uc.dwStructSize = sizeof(uc);
   char hostBuf[256]  = {};
   char pathBuf[2048] = {};
   uc.lpszHostName     = hostBuf;
   uc.dwHostNameLength = sizeof(hostBuf);
   uc.lpszUrlPath      = pathBuf;
   uc.dwUrlPathLength  = sizeof(pathBuf);
   if (!InternetCrackUrlA(url, 0, 0, &uc)) { g_lua.pushnil(L); return 1; }

   const BOOL  isHttps = (uc.nScheme == INTERNET_SCHEME_HTTPS);
   const DWORD flags   = INTERNET_FLAG_RELOAD |
                         (isHttps ? INTERNET_FLAG_SECURE |
                                    INTERNET_FLAG_IGNORE_CERT_CN_INVALID |
                                    INTERNET_FLAG_IGNORE_CERT_DATE_INVALID : 0);
   const INTERNET_PORT port = uc.nPort ? uc.nPort
                            : (isHttps ? INTERNET_DEFAULT_HTTPS_PORT
                                       : INTERNET_DEFAULT_HTTP_PORT);

   HINTERNET hNet = http_open_session();
   if (!hNet) { g_lua.pushnil(L); return 1; }

   HINTERNET hConn = InternetConnectA(hNet, hostBuf, port,
                                      nullptr, nullptr, INTERNET_SERVICE_HTTP, 0, 0);
   if (!hConn) { InternetCloseHandle(hNet); g_lua.pushnil(L); return 1; }

   const char* path = pathBuf[0] ? pathBuf : "/";
   HINTERNET hReq = HttpOpenRequestA(hConn, "POST", path,
                                     nullptr, nullptr, nullptr, flags, 0);
   if (!hReq) { InternetCloseHandle(hConn); InternetCloseHandle(hNet); g_lua.pushnil(L); return 1; }

   static const char headers[] = "Content-Type: application/json\r\n";
   if (!HttpSendRequestA(hReq, headers, (DWORD)(sizeof(headers) - 1),
                         (LPVOID)body, body ? (DWORD)bodyLen : 0)) {
      InternetCloseHandle(hReq); InternetCloseHandle(hConn); InternetCloseHandle(hNet);
      g_lua.pushnil(L); return 1;
   }

   char chunk[4096];
   DWORD bytes_read = 0;
   size_t total = 0;
   size_t capacity = 65536;
   char* buf = (char*)malloc(capacity);
   if (!buf) { InternetCloseHandle(hReq); InternetCloseHandle(hConn); InternetCloseHandle(hNet); g_lua.pushnil(L); return 1; }

   while (InternetReadFile(hReq, chunk, sizeof(chunk), &bytes_read) && bytes_read > 0) {
      if (total + bytes_read > capacity) {
         capacity *= 2;
         char* newbuf = (char*)realloc(buf, capacity);
         if (!newbuf) { free(buf); InternetCloseHandle(hReq); InternetCloseHandle(hConn); InternetCloseHandle(hNet); g_lua.pushnil(L); return 1; }
         buf = newbuf;
      }
      memcpy(buf + total, chunk, bytes_read);
      total += bytes_read;
   }

   InternetCloseHandle(hReq);
   InternetCloseHandle(hConn);
   InternetCloseHandle(hNet);

   g_lua.pushlstring(L, buf, total);
   free(buf);
   return 1;
}

// ---------------------------------------------------------------------------
// GetCharacterWeapon(charIndex [, channel]) - returns the ODF name of the
// currently selected weapon in a given weapon channel.
//
// @param #int  charIndex   Integer character unit index (0-based)
// @param #int  channel     Weapon channel (default 0), nil outside 0-1.
//                          0 = primary weapon channel (key 1)
//                          1 = secondary weapon channel (key 2)
// @return #string          ODF name (e.g. "rep_weap_dc-15s_blaster_carbine"), or nil.
//
// Resolution chain:
//   mCharacterStructArray + charIndex * 0x1B0  → charSlot
//   *(charSlot + 0x148)                        → intermediate
//   intermediate + 0x18                        → Controllable*
//   *(Controllable + 0x4D8 + slotIdx*4)        → Weapon* (slot array, up to 8)
//   *(Weapon + 0x060)                          → WeaponClass*
//   WeaponClass + 0x30                         → char[] ODF name
//
// Weapon index tracking (confirmed via runtime testing):
//   ctrl+0x4F8 is an array of bytes, one per weapon channel.
//   Each byte is a direct index into the weapon slot array at ctrl+0x4D8.
//     ctrl+0x4F8 byte[0] = selected slot index for channel 0 (primary)
//     ctrl+0x4F9 byte[1] = selected slot index for channel 1 (secondary)
//   General: *(uint8_t*)(ctrl + 0x4F8 + channel) = slot index for that channel.
// ---------------------------------------------------------------------------
static int lua_GetCharacterWeapon(lua_State* L)
{
   const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
   auto res = [=](uintptr_t a) -> uintptr_t { return a - kUnrelocatedBase + base; };

   if (!g_addr->char_array_base || !g_addr->max_chars) { g_lua.pushnil(L); return 1; }
   if (!g_lua.isnumber(L, 1)) { g_lua.pushnil(L); return 1; }

   const int charIndex = g_lua.tointeger(L, 1);
   const int maxChars  = *(int*)res(g_addr->max_chars);
   if (charIndex < 0 || charIndex >= maxChars) { g_lua.pushnil(L); return 1; }

   const uintptr_t arrayBase = *(uintptr_t*)res(g_addr->char_array_base);
   if (!arrayBase) { g_lua.pushnil(L); return 1; }

   // mWeaponIndex has two entries; higher channels would read the bytes after it.
   const int channel = (g_lua.gettop(L) >= 2 && g_lua.isnumber(L, 2))
                       ? g_lua.tointeger(L, 2) : 0;
   if (channel < 0 || channel > 1) { g_lua.pushnil(L); return 1; }

   __try {
      char* charSlot = (char*)arrayBase + charIndex * layout::Character::kSize;
      // The pointer at slot+0x148 is the soldier's Controllable sub-object
      // (struct+0x240) — the "entity" view all g_soldier offsets are based on.
      // (The historical "+0x18 ctrl view" read the same fields at aliased
      // offsets; direct entity offsets are what the per-build layout stores.)
      uintptr_t entity = *(uintptr_t*)(charSlot + layout::Character::kUnit);
      if (!entity) { g_lua.pushnil(L); return 1; }

      // Read the slot index for the requested channel (mWeaponIndex[channel])
      uint8_t slotIdx = 0;
      __try { slotIdx = *(uint8_t*)(entity + g_soldier->weaponIndexMap + channel); }
      __except (EXCEPTION_EXECUTE_HANDLER) { g_lua.pushnil(L); return 1; }

      if (slotIdx >= 8) { g_lua.pushnil(L); return 1; }

      // Read weapon pointer from mWeapon[slotIdx]
      uintptr_t wpn = 0;
      __try { wpn = *(uintptr_t*)(entity + g_soldier->weaponArray + slotIdx * 4); }
      __except (EXCEPTION_EXECUTE_HANDLER) { g_lua.pushnil(L); return 1; }
      if (!wpn || wpn == 0xCDCDCDCDu) { g_lua.pushnil(L); return 1; }

      // Read WeaponClass pointer
      uintptr_t wc = 0;
      __try { wc = *(uintptr_t*)(wpn + layout::Weapon::kStart); }
      __except (EXCEPTION_EXECUTE_HANDLER) { g_lua.pushnil(L); return 1; }
      if (!wc) { g_lua.pushnil(L); return 1; }

      // Read ODF name from WeaponClass
      const char* odfName = (const char*)(wc + 0x30);
      g_lua.pushlstring(L, odfName, strlen(odfName));
      return 1;
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      g_lua.pushnil(L);
      return 1;
   }
}

// WeaponClass ODF-name hash, Factory_data+0x14.  Build-invariant - see the walk
// in lua_SetCharacterWeapon for why this is used instead of a name string.
constexpr uint32_t kWcNameHash = 0x18;

// ---------------------------------------------------------------------------
// SetCharacterWeapon(charIndex, odfName [, channel]) - replaces the currently
// active weapon in a channel with a different already-loaded weapon ODF.
//
// @param #int    charIndex   Integer character unit index (0-based)
// @param #string odfName     ODF name to switch to (must be loaded by the level)
// @param #int    channel     Weapon channel (default 0): 0=primary, 1=secondary
// @return #bool              1 on success, nil on failure.
//
// v6 (2026-07-18) — engine-native rebuild. See docs/RE/CharacterWeaponSystem.md
// "SetCharacterWeapon v6" for the full RE trail (Phantom-build PDB).
//
// TODO:
// - Melee-family swaps are untested and likely unsafe even with the animmap
//   guard: WeaponMeleeThrow caches m_pPrimaryWeapon (its WeaponMelee partner)
//   once at build time and derefs it unconditionally, so swapping a hero's
//   saber out dangles the throw weapon's back-pointer. Refuse melee swaps if
//   this ever matters.
//
// Do exactly what EntitySoldier's constructor does for one slot
// (modtools ctor 0x5339d0, weapon-build loop at 0x533e20):
//   1. Resolve entity (EntitySoldier's Controllable sub-object, struct+0x240).
//   2. Find the target WeaponClass in the Factory list (unchanged walk).
//   3. Fill a stack WeaponDesc {owner, aimer, trigger, reload, numClips, 0, 0}
//      with the same pointers the ctor passes.
//   4. newWpn = foundWc->vtbl[+0x8](&desc)  — WeaponClass::Build. The Weapon
//      ctor allocates its own AmmoCounter/EnergyBar, binds the soldier's
//      aimer to itself, and resolves its own animation MAP from the owner's
//      animation bank (Weapon.cpp:0x60 "Weapon failed to find animmap %s_%s").
//   5. Swap the new pointer into the soldier's mWeapon slot (entity+0x4F0[slot],
//      the same memory GetCharacterWeapon reads) and the slot-0 cache.
//   6. Re-run the ctor's aimer fixup: Aimer::SetWeapon(mWeapon[weaponIndex[0]]).
//   7. Destroy the old weapon via its own virtual deleting dtor (vtbl[0](1)) —
//      identical to ~EntitySoldier's teardown. Frees back to Weapon pool.
// Net pool delta: +1 Build, -1 destroy = zero. No vtable hacks, no donor
// weapon scan, no MAP bookkeeping.
//
// Guardrails:
//   - Refused in multiplayer (weapon rebuild is not replicated).
//   - Refused for slots using WeaponShareAmmo / WeaponShareEnergy (the desc
//     would need the shared refcounted counter wired through; not done yet).
//   - Refused when the unit's animation bank has no animmap for the new
//     weapon (Weapon ctor leaves MAP=-1 → anim system crash); the freshly
//     built weapon is destroyed again and the old one stays in the slot.
//   - Only works with ODFs already loaded in memory (in a .lvl the level read).
static int lua_SetCharacterWeapon(lua_State* L)
{
   const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
   auto res = [=](uintptr_t a) -> uintptr_t { return a - kUnrelocatedBase + base; };

   if (!g_addr->char_array_base || !g_addr->max_chars || !g_addr->game_log ||
       !g_addr->net_in_shell || !g_addr->aimer_set_weapon) {
      g_lua.pushnil(L); return 1;
   }
   // get_gamelog() stamps our own file and line on each line; the raw
   // RedWarning::LogMessage inherits whatever context the last engine warning
   // left behind, which is why these used to show up under unrelated files.
   const auto fn_GameLog = get_gamelog();
   const SoldierLayout& lay = *g_soldier;

   if (!g_lua.isnumber(L, 1)) { g_lua.pushnil(L); return 1; }

   const int   charIndex = g_lua.tointeger(L, 1);
   const char* targetOdf = g_lua.tolstring(L, 2, nullptr);
   if (!targetOdf || targetOdf[0] == '\0') { g_lua.pushnil(L); return 1; }

   // Only channels 0/1 exist: the engine's mWeaponIndex map (struct+0x750) and
   // the trigger array the WeaponDesc points into (struct+0x278) are 2 entries.
   const int channel = (g_lua.gettop(L) >= 3 && g_lua.isnumber(L, 3))
                       ? g_lua.tointeger(L, 3) : 0;
   if (channel < 0 || channel > 1) { g_lua.pushnil(L); return 1; }

   // Multiplayer guard — the rebuild is purely local and would desync peers.
   // Exact game idiom (EntitySoldier ctor @0x534001): netEnabledNext only counts
   // while in the shell; in-game the authority is netEnabled. A plain OR of both
   // flags wrongly blocked SP missions when netEnabledNext lingered from the shell.
   {
      const uint8_t inShell = *(uint8_t*)res(g_addr->net_in_shell);
      const uint8_t net = inShell ? *(uint8_t*)res(g_addr->net_enabled_next)
                                  : *(uint8_t*)res(g_addr->net_enabled);
      if (net) {
         fn_GameLog("SetCharacterWeapon: disabled in multiplayer (inShell=%d).\n", (int)inShell);
         g_lua.pushnil(L);
         return 1;
      }
   }

   const int maxChars = *(int*)res(g_addr->max_chars);
   if (charIndex < 0 || charIndex >= maxChars) {
      fn_GameLog("SetCharacterWeapon: charIndex %d out of range (max %d).\n", charIndex, maxChars);
      g_lua.pushnil(L); return 1;
   }

   const uintptr_t arrayBase = *(uintptr_t*)res(g_addr->char_array_base);
   if (!arrayBase) {
      fn_GameLog("SetCharacterWeapon: character array not initialised.\n");
      g_lua.pushnil(L); return 1;
   }

   __try {
      char* charSlot     = (char*)arrayBase + charIndex * layout::Character::kSize;
      char* intermediate = *(char**)(charSlot + layout::Character::kUnit);
      if (!intermediate) {
         fn_GameLog("SetCharacterWeapon: char %d has no unit (intermediate null).\n", charIndex);
         g_lua.pushnil(L); return 1;
      }

      // The pointer stored at charSlot+0x148 IS the soldier's Controllable
      // sub-object (struct+0x240) — the exact pointer the engine passes as
      // WeaponDesc::mOwner. Proof: GetCharacterWeapon's proven reads at
      // (intermediate+0x18)+0x4D8 / +0x4F8 land on struct+0x730 (mWeapon) and
      // struct+0x750 (mWeaponIndex) from the ctor disasm — the "+0x18 ctrl
      // view" was an accidental offset onto the same object. The old
      // *(ctrl+0x290) "entity" pointer was NOT the soldier (its +0x4F0 reads
      // NULL for the player) and is no longer used.
      const uintptr_t entity = (uintptr_t)intermediate;
      if (entity == 0xCDCDCDCDu) {
         fn_GameLog("SetCharacterWeapon: char %d controllable ptr invalid.\n", charIndex);
         g_lua.pushnil(L); return 1;
      }

      // Channel → slot index. mWeaponIndex[2] (modtools struct+0x750 / Steam
      // +0x740, filled by the ctor's channel-mapping loop; 0xFF = empty).
      uint8_t slotIdx = 0xFF;
      __try { slotIdx = *(uint8_t*)(entity + lay.weaponIndexMap + channel); }
      __except (EXCEPTION_EXECUTE_HANDLER) { g_lua.pushnil(L); return 1; }
      if (slotIdx >= 8) {
         fn_GameLog("SetCharacterWeapon: char %d channel %d has no weapon slot (idx=%d).\n",
                    charIndex, channel, (int)slotIdx);
         g_lua.pushnil(L); return 1;
      }

      // Old weapon from the ENTITY-side mWeapon[] array — the array the ctor
      // fills with Build results and ~EntitySoldier frees.
      uintptr_t oldWpn = 0;
      __try { oldWpn = *(uintptr_t*)(entity + lay.weaponArray + slotIdx * 4); }
      __except (EXCEPTION_EXECUTE_HANDLER) { g_lua.pushnil(L); return 1; }
      if (!oldWpn || oldWpn == 0xCDCDCDCDu) {
         fn_GameLog("SetCharacterWeapon: char %d slot %d entity-side weapon invalid (0x%08x).\n",
                    charIndex, (int)slotIdx, (unsigned)oldWpn);
         g_lua.pushnil(L); return 1;
      }

      uintptr_t startWc = 0;
      __try { startWc = *(uintptr_t*)(oldWpn + layout::Weapon::kStart); }
      __except (EXCEPTION_EXECUTE_HANDLER) { g_lua.pushnil(L); return 1; }
      if (!startWc || startWc == 0xCDCDCDCDu) {
         fn_GameLog("SetCharacterWeapon: char %d slot %d WeaponClass ptr invalid.\n",
                    charIndex, (int)slotIdx);
         g_lua.pushnil(L); return 1;
      }

      // Walk the WeaponClass global linked list.
      // Flink/Blink (WC+0x008/0x00C) store adjacentWC+0x004; subtract 4 when
      // following.  Verified identical on all three builds - the Factory base
      // occupies WC+0x04..+0x1F and links exactly this way (modtools ctor
      // 0x00406154, Steam 0x0067BFE0).
      //
      // Match on the ODF name HASH at WC+0x18 rather than on the name string.
      // Factory::Factory(this[, parent], nameHash) stores the hash at
      // Factory_data+0x14 = WC+0x18 on every build, and ODF-derived classes
      // arrive at the same field through clone() -> the copy ctor.
      //
      // WC+0x30 (char[32] mFilename) does exist and is populated on all three
      // builds, so a string compare would also work; the hash is preferred
      // because it is exact, needs no case folding, and is not subject to the
      // 32-character truncation of that buffer.
      //
      // Hash matching is exact, so the full ODF name is required; the old
      // suffix shorthand ("rifle" for "rep_weap_inf_rifle") is gone.
      const uint32_t targetHash = pbl_hash(targetOdf);

      uintptr_t foundWc  = 0;
      uintptr_t searchWc = startWc;
      for (int guard = 0; guard < 512; guard++) {
         __try {
            if (*(uint32_t*)(searchWc + kWcNameHash) == targetHash) { foundWc = searchWc; break; }
            uintptr_t linkRaw = *(uintptr_t*)(searchWc + 0x008);
            if (!linkRaw || linkRaw == 0xCDCDCDCDu || linkRaw < 0x01000000u) break;
            uintptr_t nextWc = linkRaw - 0x004;
            if (nextWc == startWc) break;
            searchWc = nextWc;
         }
         __except (EXCEPTION_EXECUTE_HANDLER) { break; }
      }

      if (!foundWc) {
         // Walk backwards in case the target is behind the start node.
         searchWc = startWc;
         for (int guard = 0; guard < 512; guard++) {
            __try {
               uintptr_t linkRaw = *(uintptr_t*)(searchWc + 0x00C);
               if (!linkRaw || linkRaw == 0xCDCDCDCDu || linkRaw < 0x01000000u) break;
               uintptr_t prevWc = linkRaw - 0x004;
               if (prevWc == startWc) break;
               if (*(uint32_t*)(prevWc + kWcNameHash) == targetHash) { foundWc = prevWc; break; }
               searchWc = prevWc;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { break; }
         }
      }

      if (!foundWc) {
         fn_GameLog("SetCharacterWeapon: '%s' not found in loaded WeaponClass list.\n", targetOdf);
         g_lua.pushnil(L);
         return 1;
      }

      // Already holding the requested class - nothing to do.
      if (foundWc == startWc) {
         fn_GameLog("SetCharacterWeapon: char %d already holds '%s' - no-op.\n", charIndex, targetOdf);
         g_lua.pushnumber(L, 1); return 1;
      }

      // EntitySoldierClass — per-slot loadout config (offsets from the ctor's
      // weapon-build loop; modtools @0x533e20, Steam @0x4dedc0 — see
      // entity_layout.hpp).
      uintptr_t esClass = 0;
      __try { esClass = *(uintptr_t*)(entity + lay.classPtr); } __except(EXCEPTION_EXECUTE_HANDLER) {}
      if (!esClass || esClass == 0xCDCDCDCDu) {
         fn_GameLog("SetCharacterWeapon: char %d EntitySoldierClass ptr invalid.\n", charIndex);
         g_lua.pushnil(L); return 1;
      }

      int8_t weaponCount = 0;
      __try { weaponCount = *(int8_t*)(esClass + lay.clsWeaponCount); } __except(EXCEPTION_EXECUTE_HANDLER) {}
      if ((int)slotIdx >= (int)weaponCount) {
         fn_GameLog("SetCharacterWeapon: slot %d >= class weapon count %d.\n",
                    (int)slotIdx, (int)weaponCount);
         g_lua.pushnil(L); return 1;
      }

      // WeaponAmmo config for this slot:
      //   negative       → WeaponShareAmmo with another slot   → refuse
      //   bit 0x40000000 → WeaponShareEnergy with another slot → refuse
      //   bit 0x20000000 → infinite ammo → mNumClips = INT_MAX (ctor @0x533eb6)
      int32_t ammoVal = 0;
      __try { ammoVal = *(int32_t*)(esClass + lay.clsWeaponAmmo + slotIdx * 4); } __except(EXCEPTION_EXECUTE_HANDLER) {}
      if (ammoVal < 0 || (ammoVal & 0x40000000)) {
         fn_GameLog("SetCharacterWeapon: slot %d uses WeaponShareAmmo/Energy - not supported.\n",
                    (int)slotIdx);
         g_lua.pushnil(L);
         return 1;
      }
      const int32_t numClips = (ammoVal & 0x20000000) ? 0x7FFFFFFF : (ammoVal & 0xFFFFFF);

      int8_t chSlot = (int8_t)channel;
      __try { chSlot = *(int8_t*)(esClass + lay.clsWeaponChannel + slotIdx); } __except(EXCEPTION_EXECUTE_HANDLER) {}
      if (chSlot < 0 || chSlot > 1) chSlot = (int8_t)channel;

      // WeaponDesc — identical to the stack desc the soldier ctor builds
      // (@0x533e30..0x533ee6). All pointers reference the live soldier, so the
      // new Weapon reads the same triggers/aimer the old one did.
      struct WeaponDescRaw {
         uintptr_t owner;             // Controllable sub-object (== entity)
         uintptr_t aimer;             // entity+0x2D0 (struct+0x510)
         uintptr_t trigger;           // entity+0x38+ch*4 (struct+0x278)
         uintptr_t reload;            // entity+0x40 (struct+0x280)
         int32_t   numClips;
         uintptr_t sharedAmmoCounter; // unused (shared slots refused above)
         uintptr_t sharedEnergyBar;   // unused
      } desc = {};

      desc.owner    = entity;
      desc.aimer    = entity + lay.aimer;
      desc.numClips = numClips;

      uint8_t dualFlag = 0;
      __try { dualFlag = *(uint8_t*)(entity + lay.dualWieldFlag); } __except(EXCEPTION_EXECUTE_HANDLER) {}
      if ((dualFlag & 1) && chSlot == 1) {
         // Dual-wield secondary: fire trigger is the reload trigger, no reload.
         desc.trigger = entity + 0x40;
         desc.reload  = 0;
      } else {
         desc.trigger = entity + 0x38 + (uintptr_t)chSlot * 4;
         desc.reload  = entity + 0x40;
      }

      // Point the aimer back at the primary-channel active weapon. Used both as
      // the ctor's post-loop fixup and as the rollback for every path that
      // abandons a build — Weapon::Weapon calls Aimer::SetWeapon(mAimer, this)
      // unconditionally, so any failure after that leaves the aimer bound to a
      // weapon that is about to go away.
      auto rebindAimer = [&]() {
         __try {
            int8_t aimSlot = *(int8_t*)(entity + lay.weaponIndexMap);
            if (aimSlot >= 0 && aimSlot < 8) {
               uintptr_t aimWpn = *(uintptr_t*)(entity + lay.weaponArray + aimSlot * 4);
               if (aimWpn && aimWpn != 0xCDCDCDCDu) {
                  typedef void (__thiscall* AimerSetWeapon_t)(uintptr_t aimer, uintptr_t w);
                  ((AimerSetWeapon_t)res(g_addr->aimer_set_weapon))(entity + lay.aimer, aimWpn);
               }
            }
         } __except(EXCEPTION_EXECUTE_HANDLER) {}
      };

      // Animmap PRE-FLIGHT — run the exact lookup Weapon::Weapon does, before
      // anything is constructed.
      //
      // The ctor (modtools 0x61d410) does Aimer::SetWeapon(mAimer, this) FIRST
      // and only then resolves the MAP, so a post-hoc check is always too late:
      //   * the soldier's aimer is already bound to the new weapon, and
      //   * WeaponMeleeClass::Build (Phantom 0x7c1fd5) copies the freshly built
      //     instance's MAP into the SHARED class — m_eDeflectMatrixMap = -1 plus
      //     _InitDeflectMatrices(this, -1) — which no rollback can undo and which
      //     poisons every later spawn of that ODF.  It also derefs the instance
      //     with no null check, so a pool miss access-violates inside Build; our
      //     __except then reports "Build failed" while a live, aimer-bound weapon
      //     is left orphaned in the pool (it crashes later in WeaponMelee::Update
      //     once its owner at Weapon+0x6C is freed).
      //
      //   WeaponClass::mSoldierAnimationWeapon = +0x20 (INVALID_WEAPON = -1)
      //   Controllable vtbl +0x4C              = GetAnimationBank (BANK, -1 = none)
      //   SoldierAnimationBank::FindMap(BANK, WEAPON) -> MAP (-1 = miss)
      // Both are skipped by the ctor when the weapon or bank is -1, so mirror that.
      {
         int32_t wantWeapon = -1;
         __try { wantWeapon = *(int32_t*)(foundWc + 0x20); } __except(EXCEPTION_EXECUTE_HANDLER) {}

         if (wantWeapon != -1 && g_addr->get_weapon_anim_map) {
            int32_t bank = -1;
            __try {
               typedef int32_t (__thiscall* GetAnimBank_t)(uintptr_t ctrl);
               GetAnimBank_t fnBank = *(GetAnimBank_t*)(*(uintptr_t*)entity + 0x4C);
               bank = fnBank(entity);
            } __except(EXCEPTION_EXECUTE_HANDLER) { bank = -1; }

            if (bank != -1) {
               // SoldierAnimationBank::FindMap is __cdecl on the debug build and
               // __fastcall (ECX=bank, EDX=weapon) on both release builds - the
               // release compiler picked up the register convention for this
               // free function.  Calling it __cdecl on retail leaves ECX/EDX
               // holding whatever the last call left there, so the table scan
               // compares garbage, returns INVALID_MAP, and the swap is refused
               // with a "has no animmap" that is not true.
               //
               // The engine's own call site is Weapon::Weapon (Steam 0x00677A19):
               //     MOV EDX,[EBX+0x20]   ; WeaponClass::mSoldierAnimationWeapon
               //     MOV ECX,EAX          ; owner->GetAnimationBank() result
               //     CALL <FindMap>
               //     MOV [ESI+0xC8],EAX   ; -> Weapon::mWeaponAnimationMap
               // which also confirms +0x20 and the +0x4C bank virtual on retail.
               int32_t preMap = -1;
               __try {
                  typedef int32_t (__cdecl*    FindMapCdecl_t)(int32_t bank, int32_t weapon);
                  typedef int32_t (__fastcall* FindMapFast_t)(int32_t bank, int32_t weapon);
                  const uintptr_t fnFindMap = (uintptr_t)res(g_addr->get_weapon_anim_map);
                  preMap = (g_build == GameBuild::Modtools)
                           ? ((FindMapCdecl_t)fnFindMap)(bank, wantWeapon)
                           : ((FindMapFast_t)fnFindMap)(bank, wantWeapon);
               } __except(EXCEPTION_EXECUTE_HANDLER) { preMap = -1; }

               if (preMap == -1) {
                  fn_GameLog("SetCharacterWeapon: '%s' has no animmap in char %d's animation bank "
                             "(bank=%d weapon=%d) - refused before build.\n",
                             targetOdf, charIndex, bank, wantWeapon);
                  g_lua.pushnil(L);
                  return 1;
               }
            }
         }
      }

      // Build the replacement — WeaponClass::Build, vtable slot +0x8,
      // __thiscall(WeaponDesc*), returns Weapon* (NULL if the pool is full).
      // The Weapon ctor allocates its own AmmoCounter/EnergyBar, binds the
      // soldier's aimer to itself, and resolves its animation MAP from the
      // owner's animation bank — no manual MAP fixing needed.
      uintptr_t newWpn = 0;
      __try {
         typedef uintptr_t (__thiscall* WcBuild_t)(uintptr_t wc, WeaponDescRaw* d);
         WcBuild_t fnBuild = *(WcBuild_t*)(*(uintptr_t*)foundWc + 0x8);
         newWpn = fnBuild(foundWc, &desc);
      } __except(EXCEPTION_EXECUTE_HANDLER) { newWpn = 0; }
      if (!newWpn) {
         // Build may have run the ctor before failing (or faulted partway through
         // it), in which case the aimer is already pointing at a weapon we will
         // never install. Put it back on the weapon the soldier still holds.
         rebindAimer();
         fn_GameLog("SetCharacterWeapon: WeaponClass::Build failed for '%s'.\n", targetOdf);
         g_lua.pushnil(L);
         return 1;
      }

      // Animmap guard. The Weapon ctor resolves its animation MAP from the
      // owner's animation bank; when the bank has no map for this weapon it
      // warns ("Weapon failed to find animmap %s_%s", Weapon.cpp:96) and
      // leaves weapon+0xC8 = -1, which the animation system later crashes on.
      // Roll back: destroy the new weapon and rebind the aimer (the Weapon
      // ctor pointed it at newWpn; ~Weapon does not touch it, so destroy-then-
      // rebind is safe). The entity slots are still untouched here, so the
      // unit simply keeps its old weapon.
      int32_t newMap = -1;
      __try { newMap = *(int32_t*)(newWpn + layout::Weapon::kSoldierAnimationMap); } __except(EXCEPTION_EXECUTE_HANDLER) {}
      // Offhand weapons (grenades, dispensers) legitimately have no animmap: the
      // stock thermal detonator in the secondary channel already lives with
      // MAP = -1. Only refuse when the weapon we replace HAS a map, i.e. when
      // we would take a working animation away from the slot.
      int32_t oldMap = 0;
      __try { oldMap = *(int32_t*)(oldWpn + layout::Weapon::kSoldierAnimationMap); } __except(EXCEPTION_EXECUTE_HANDLER) {}
      fn_GameLog("SetCharacterWeapon: '%s' newMap=%d oldMap=%d channel=%d\n",
                 targetOdf, newMap, oldMap, channel);
      if (newMap == -1 && oldMap != -1) {
         __try {
            typedef void (__thiscall* WpnDelete_t)(uintptr_t w, uint32_t flags);
            ((WpnDelete_t)(**(uintptr_t**)newWpn))(newWpn, 1);
         } __except(EXCEPTION_EXECUTE_HANDLER) {}
         rebindAimer();
         fn_GameLog("SetCharacterWeapon: '%s' has no animmap in char %d's animation bank - swap refused.\n",
                    targetOdf, charIndex);
         g_lua.pushnil(L);
         return 1;
      }

      // Swap into the mWeapon slot the engine owns. This is the same memory
      // GetCharacterWeapon reads, so Get reflects the swap immediately — there
      // is no second array.
      __try { *(uintptr_t*)(entity + lay.weaponArray + slotIdx * 4) = newWpn; } __except(EXCEPTION_EXECUTE_HANDLER) {}

      // Retarget the cached slot-0 weapon pointer (modtools struct+0x718, the
      // old "ctrl+0x4C0 always equals slot[0]" observation) if it held the old
      // one.  Compare-guarded, so a stale offset guess is a no-op.
      __try {
         uintptr_t* cache = (uintptr_t*)(entity + lay.slot0Cache);
         if (*cache == oldWpn) *cache = newWpn;
      } __except(EXCEPTION_EXECUTE_HANDLER) {}

      // The Weapon ctor unconditionally bound the aimer to the NEW weapon.
      // Re-run the soldier ctor's post-loop fixup (modtools @0x533fee, Steam
      // @0x4defa2): point the aimer back at the primary-channel active weapon
      // (which is newWpn itself when that is the slot we just swapped).
      rebindAimer();

      // The Weapon ctor leaves the "currently drawn" flag (weapon+0xAC bit 2)
      // CLEAR — Weapon::Update's IDLE state refuses to even read the fire
      // trigger until Weapon::Select sets it (Select = vtable+0x74; body sets
      // field_0xAC = (field_0xAC & 0xFFFFFE07) | 4; slot verified identical in
      // modtools @0x61ba80 and Phantom @0x7af080). The game only calls Select
      // on a weapon switch, so without this the swapped-in weapon can't fire
      // until the player cycles to the other channel and back. Call it now if
      // the swapped slot is the one currently drawn (entity+0x512 low nibble =
      // active slot index, the byte UpdateIndirect reads). Second arg true =
      // skip the weapon-change sound.
      __try {
         uint8_t active = *(uint8_t*)(entity + lay.weaponIndex) & 0x0F;
         if (active == (uint8_t)slotIdx) {
            typedef void (__thiscall* WpnSelect_t)(uintptr_t w, uint32_t chan, uint32_t quiet);
            WpnSelect_t fnSelect = *(WpnSelect_t*)(*(uintptr_t*)newWpn + 0x74);
            fnSelect(newWpn, (uint32_t)channel, 1u);
         }
      } __except(EXCEPTION_EXECUTE_HANDLER) {}

      // Destroy the old weapon LAST, after every reference is rebound.
      // Virtual deleting dtor (vtbl[0](1)) — the same call ~EntitySoldier makes.
      // Frees the item back to the Weapon pool; ~Weapon stops its sounds and
      // releases its refcounted AmmoCounter/EnergyBar. Net pool delta: zero.
      __try {
         typedef void (__thiscall* WpnDelete_t)(uintptr_t w, uint32_t flags);
         ((WpnDelete_t)(**(uintptr_t**)oldWpn))(oldWpn, 1);
      } __except(EXCEPTION_EXECUTE_HANDLER) {}

      // First-person view-model cache. FirstPersonRenderable keeps the active
      // Weapon* at +0x1600 (mCurrentWeapon) and its holster transition keeps
      // rendering it via vtbl+0x8C after a weapon change — with the old
      // weapon destroyed that call lands in freed pool memory (RenderSoldier
      // crash at 0x4aa59c, EIP=0). Clearing it is engine-safe: RenderSoldier
      // null-checks the field, and UpdateSoldier's holster path requires it
      // to be non-null, so the swapped-in weapon is adopted immediately.
      __try {
         if (g_addr->fp_renderable) {
            // mCurrentWeapon +0x1600 is build-invariant (Phantom PDB struct).
            uintptr_t fpr = *(uintptr_t*)res(g_addr->fp_renderable);
            if (fpr && fpr != 0xCDCDCDCDu && *(uintptr_t*)(fpr + 0x1600) == oldWpn)
               *(uintptr_t*)(fpr + 0x1600) = 0;
         }
      } __except(EXCEPTION_EXECUTE_HANDLER) {}

      // Instant animation switch. The Weapon ctor already wrote the correct MAP
      // into newWpn+0xC8 (resolved from the owner's animation bank), and
      // EntitySoldier::UpdateIndirect (0x0053b920) re-asserts it every frame —
      // both sides now agree, so no oscillation is possible. Calling SWAM here
      // just makes the stance change this frame instead of next.
      __try {
         uintptr_t animator = *(uintptr_t*)(entity + lay.animator);
         int32_t   newMap   = *(int32_t*)(newWpn + layout::Weapon::kSoldierAnimationMap);
         if (animator && animator != 0xCDCDCDCDu && newMap != -1 && g_addr->set_weapon_anim_map) {
            typedef void (__thiscall* SetWeaponAnimMap_t)(void*, int32_t);
            ((SetWeaponAnimMap_t)res(g_addr->set_weapon_anim_map))((void*)animator, newMap);
         }
      } __except(EXCEPTION_EXECUTE_HANDLER) {}

      fn_GameLog("SetCharacterWeapon: char %d ch %d slot[%d] rebuilt -> '%s' (wpn=0x%08x map=%d)\n",
                 charIndex, (int)chSlot, (int)slotIdx, targetOdf,
                 (unsigned)newWpn, *(int32_t*)(newWpn + layout::Weapon::kSoldierAnimationMap));

      g_lua.pushnumber(L, 1);
      return 1;
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      g_lua.pushnil(L);
      return 1;
   }
}


// ---------------------------------------------------------------------------
// Resolve (charIndex, channel) → active Weapon* for that channel.
// Returns nullptr on any failure. Shared by GetWeaponAmmo / SetWeaponAmmo.
//
// Confirmed chain (see docs/RE/CharacterWeaponSystem.md):
//   charArray + idx*0x1B0     → charSlot
//   *(charSlot + 0x148)       → intermediate
//   intermediate + 0x18       → Controllable*
//   *(ctrl + 0x4F8 + channel) → uint8 slotIdx
//   *(ctrl + 0x4D8 + slotIdx*4) → Weapon*
// ---------------------------------------------------------------------------
static uintptr_t resolve_active_weapon(int charIndex, int channel)
{
   const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
   auto res = [=](uintptr_t a) -> uintptr_t { return a - kUnrelocatedBase + base; };

   if (!g_addr->char_array_base || !g_addr->max_chars) return 0;

   const int maxChars = *(int*)res(g_addr->max_chars);
   if (charIndex < 0 || charIndex >= maxChars) return 0;
   if (channel < 0 || channel > 1) return 0;   // mWeaponIndex has two entries

   const uintptr_t arrayBase = *(uintptr_t*)res(g_addr->char_array_base);
   if (!arrayBase) return 0;

   __try {
      char* charSlot = (char*)arrayBase + charIndex * layout::Character::kSize;
      uintptr_t entity = *(uintptr_t*)(charSlot + layout::Character::kUnit);
      if (!entity) return 0;

      uint8_t slotIdx = *(uint8_t*)(entity + g_soldier->weaponIndexMap + channel);
      if (slotIdx >= 8) return 0;

      uintptr_t wpn = *(uintptr_t*)(entity + g_soldier->weaponArray + slotIdx * 4);
      if (!wpn || wpn == 0xCDCDCDCDu) return 0;
      return wpn;
   }
   __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// ---------------------------------------------------------------------------
// GetWeaponAmmo(charIndex [, channel]) - returns ammo state for the active
// weapon in a given channel.
//
// All ammo is tracked in **clips**, not rounds. A weapon with a 50-round clip
// reading (1, 3, 4, 50) means: 1 clip currently loaded (= 50 rounds), 3 spare
// clips, max 4 clips, 50 rounds per clip → 200 rounds total.
//
// mCurClip is a float — it represents fractional clip remaining (1.0 = full,
// decreases as rounds are fired; total rounds = mCurClip * mRoundsPerClip).
//
// AmmoCounter layout (24 bytes, allocated at Weapon+0x88):
//   +0   AmmoCounterClass*  m_pClass         (→ +0 int mRoundsPerClip, +4 float mAmmoPerRound)
//   +4   float              mDefaultMaxClips
//   +8   float              mMaxClips        (current cap, can differ from default)
//   +12  float              mNumClips        (reserve clips remaining)
//   +16  float              mCurClip         (clips currently loaded — fractional)
//   +20  uint               m_uiRefCount
//
// @param #int charIndex
// @param #int channel    (default 0)
// @return curClip, numClips, maxClips, roundsPerClip  — four numbers, or nil
// ---------------------------------------------------------------------------
static int lua_GetWeaponAmmo(lua_State* L)
{
   if (!g_lua.isnumber(L, 1)) { g_lua.pushnil(L); return 1; }
   const int charIndex = g_lua.tointeger(L, 1);
   const int channel = (g_lua.gettop(L) >= 2 && g_lua.isnumber(L, 2))
                       ? g_lua.tointeger(L, 2) : 0;

   uintptr_t wpn = resolve_active_weapon(charIndex, channel);
   if (!wpn) { g_lua.pushnil(L); return 1; }

   __try {
      uintptr_t ac = *(uintptr_t*)(wpn + layout::Weapon::kAmmoCounter);
      if (!ac || ac == 0xCDCDCDCDu) { g_lua.pushnil(L); return 1; }

      float curClip  = *(float*)(ac + 0x10);
      float numClips = *(float*)(ac + 0x0C);
      float maxClips = *(float*)(ac + 0x08);

      int roundsPerClip = 0;
      uintptr_t acClass = *(uintptr_t*)(ac + 0x00);
      if (acClass && acClass != 0xCDCDCDCDu)
         roundsPerClip = *(int*)(acClass + 0x00);

      g_lua.pushnumber(L, curClip);
      g_lua.pushnumber(L, numClips);
      g_lua.pushnumber(L, maxClips);
      g_lua.pushnumber(L, roundsPerClip);
      return 4;
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      g_lua.pushnil(L);
      return 1;
   }
}

// ---------------------------------------------------------------------------
// SetWeaponAmmo(charIndex, curClip [, numClips [, channel]]) - writes ammo state.
//
// Values are in **clips**, not rounds. curClip is fractional (1.0 = full clip
// loaded, 0.5 = half a clip). numClips is the integer count of spare clips.
// Pass nil for numClips to leave it untouched. Channel defaults to 0.
//
// @return 1 on success, nil on failure
// ---------------------------------------------------------------------------
static int lua_SetWeaponAmmo(lua_State* L)
{
   if (!g_lua.isnumber(L, 1) || !g_lua.isnumber(L, 2)) { g_lua.pushnil(L); return 1; }
   const int   charIndex = g_lua.tointeger(L, 1);
   const float curClip   = (float)g_lua.tonumber(L, 2);

   const bool  haveNumClips = (g_lua.gettop(L) >= 3 && g_lua.isnumber(L, 3));
   const float numClips     = haveNumClips ? (float)g_lua.tonumber(L, 3) : 0.0f;

   const int channel = (g_lua.gettop(L) >= 4 && g_lua.isnumber(L, 4))
                       ? g_lua.tointeger(L, 4) : 0;

   uintptr_t wpn = resolve_active_weapon(charIndex, channel);
   if (!wpn) { g_lua.pushnil(L); return 1; }

   __try {
      uintptr_t ac = *(uintptr_t*)(wpn + layout::Weapon::kAmmoCounter);
      if (!ac || ac == 0xCDCDCDCDu) { g_lua.pushnil(L); return 1; }

      *(float*)(ac + 0x10) = curClip;
      if (haveNumClips) *(float*)(ac + 0x0C) = numClips;

      g_lua.pushnumber(L, 1);
      return 1;
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      g_lua.pushnil(L);
      return 1;
   }
}


// ---------------------------------------------------------------------------
// RemoveUnitClass(teamIndex, unitClass) - removes a unit class from a team.
//
// Reverse of AddUnitClass. Finds the class in the global class def list,
// locates it in the team's parallel arrays, then left-shifts remaining entries
// to preserve order and keep the arrays compact.
//
// @param #int    teamIndex   Index of team (0-based)
// @param #string unitClass   ODF class name (e.g. "imp_inf_trooper")
// ---------------------------------------------------------------------------

// Team::SetUnitClassMinMax — __thiscall on team object.
// Writes min/max into their respective parallel arrays and fires the change
// notification (thunk_FUN_00661e00).
typedef void (__thiscall* SetUnitClassMinMax_t)(void* team, int slot, int min, int max);

// Game's printf-style debug logger — same one vanilla scripts call for error output.

static int lua_RemoveUnitClass(lua_State* L)
{
   const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
   auto res = [=](uintptr_t addr) -> uintptr_t { return addr - kUnrelocatedBase + base; };

   if (!g_addr->team_array_base || !g_addr->game_log ||
       !g_addr->class_def_list || !g_addr->hash_string_thiscall)
      return 0;
   // get_gamelog() stamps our own file and line on each line; the raw
   // RedWarning::LogMessage inherits whatever context the last engine warning
   // left behind, which is why these used to show up under unrelated files.
   const auto fn_GameLog = get_gamelog();

   if (!g_lua.isnumber(L, 1)) return 0;

   // arg2 can be a string (ODF name) or a number (0-based slot index within the team).
   const bool byIndex   = g_lua.isnumber(L, 2) != 0;
   const char* unitClass = byIndex ? nullptr : g_lua.tolstring(L, 2, nullptr);
   if (!byIndex && !unitClass) return 0;

   const int teamIndex = g_lua.tointeger(L, 1);
   if (teamIndex < 0 || teamIndex >= 8) {
      fn_GameLog("RemoveUnitClass(): teamIndex %d out of range (0-7)\n", teamIndex);
      return 0;
   }

   // Get team pointer from g_ppTeams[teamIndex].
   // 0xAD5D64 is a pointer variable whose value is the team array base — two dereferences needed.
   const uintptr_t teamArrayBase = *(uintptr_t*)res(g_addr->team_array_base);
   if (!teamArrayBase) {
      fn_GameLog("RemoveUnitClass(): teams are not set up yet\n");
      return 0;
   }
   void* teamPtr = *(void**)(teamArrayBase + (uintptr_t)teamIndex * 4);
   if (!teamPtr) {
      fn_GameLog("RemoveUnitClass(): team %d is null\n", teamIndex);
      return 0;
   }

   const int classCount = *(int*)((char*)teamPtr + 0x48);
   void** classDefArr   = *(void***)((char*)teamPtr + 0x50);
   int*   minArr        = *(int**)((char*)teamPtr + 0x54);
   int*   maxArr        = *(int**)((char*)teamPtr + 0x58);
   if (classCount <= 0 || !classDefArr || !minArr || !maxArr) {
      fn_GameLog("RemoveUnitClass(): team %d has no classes\n", teamIndex);
      return 0;
   }

   int foundSlot = -1;

   if (byIndex) {
      // Direct slot index — no global registry lookup needed.
      const int classIndex = g_lua.tointeger(L, 2);
      if (classIndex < 0 || classIndex >= classCount) {
         fn_GameLog("RemoveUnitClass(): class index %d out of range (team %d has %d classes)\n",
                    classIndex, teamIndex, classCount);
         return 0;
      }
      foundSlot = classIndex;
   } else {
      // ODF name — walk g_ClassDefList to get the classDef pointer, then match by pointer.
      // Node layout: +0x04 = next, +0x0c = classDef ptr (null = end of list).
      // classDef+0x18 = integer name hash (NOT a char* — do not dereference as string).
      //
      // HashString: __thiscall, ECX = 8-byte stack buffer, stack arg = name string.
      // buf[0] is the resulting integer hash.
      typedef void* (__thiscall* HashString_t)(void* buf, const char* name);
      const auto fn_HashString = (HashString_t)res(g_addr->hash_string_thiscall);
      alignas(4) int hashBuf[2] = {};
      fn_HashString(hashBuf, unitClass);
      const int targetHash = hashBuf[0];

      uintptr_t node = *(uintptr_t*)res(g_addr->class_def_list);
      void* classDef = nullptr;
      for (int guard = 0; node && guard < 1024; ++guard) {
         void* element = *(void**)(node + 0x0c);
         if (!element) break;
         if (*(int*)((char*)element + 0x18) == targetHash) { classDef = element; break; }
         node = *(uintptr_t*)(node + 0x04);
      }
      if (!classDef) {
         fn_GameLog("RemoveUnitClass(): class \"%s\" not found in global registry (check the side's .req file)\n", unitClass);
         return 0;
      }

      for (int i = 0; i < classCount; ++i) {
         if (classDefArr[i] == classDef) { foundSlot = i; break; }
      }
      if (foundSlot < 0) {
         fn_GameLog("RemoveUnitClass(): class \"%s\" is not assigned to team %d\n", unitClass, teamIndex);
         return 0;
      }
   }

   // Left-shift removal: slide every entry after foundSlot one position left,
   // preserving spawn menu order. classCount is decremented so the freed slot
   // is immediately available for a subsequent AddUnitClass call.
   //
   // NOTE: the spawner (FUN_006470f0) pre-caches slot indices for characters
   // already queued to spawn. After the shift, any character cached on the old
   // lastSlot index will call Character::SetClass(lastSlot) → null → a one-time
   // "Trying to spawn a character with no class" warning in the log. That
   // character is skipped for that spawn tick and respawns normally on the next
   // cycle. The warning is harmless and the alternative (tombstoning) prevents
   // slot reuse, breaking the add/remove cycle.
   const int lastSlot = classCount - 1;

   for (int i = foundSlot; i < lastSlot; ++i) {
      classDefArr[i] = classDefArr[i + 1];
      minArr[i]      = minArr[i + 1];
      maxArr[i]      = maxArr[i + 1];
   }

   classDefArr[lastSlot] = nullptr;
   minArr[lastSlot]      = 0;
   maxArr[lastSlot]      = 0;
   *(int*)((char*)teamPtr + 0x48) = lastSlot;

   // Return the ODF name when called by name; nil when called by index
   // (classDef+0x18 is only a hash — the name string offset is unconfirmed).
   if (unitClass) {
      g_lua.pushlstring(L, unitClass, strlen(unitClass));
      return 1;
   }
   return 0;
}

// ---------------------------------------------------------------------------
// Async HTTP infrastructure — fire-and-forget background threads
//
// Each Async variant copies the URL/body onto the heap, spawns a thread,
// and returns immediately. The thread does the request, discards the response,
// frees the work struct, and exits. The game thread is never blocked.
// ---------------------------------------------------------------------------

struct HttpAsyncWork {
   char*  url;
   char*  body;       // null = no body
   size_t bodyLen;
   char   method[8];  // "GET", "PUT", "POST", …
   char*  headers;    // null = no extra headers
};

static void http_async_free(HttpAsyncWork* w)
{
   free(w->url);
   free(w->body);
   free(w->headers);
   free(w);
}

static DWORD WINAPI http_async_worker(LPVOID param)
{
   HttpAsyncWork* w = (HttpAsyncWork*)param;

   if (strcmp(w->method, "GET") == 0) {
      // High-level path: InternetOpenUrlA is fine for GET.
      HINTERNET hNet = http_open_session();
      if (hNet) {
         HINTERNET hUrl = InternetOpenUrlA(hNet, w->url, nullptr, 0,
                                           INTERNET_FLAG_RELOAD |
                                           INTERNET_FLAG_IGNORE_CERT_CN_INVALID |
                                           INTERNET_FLAG_IGNORE_CERT_DATE_INVALID, 0);
         if (hUrl) {
            char chunk[4096]; DWORD br = 0;
            while (InternetReadFile(hUrl, chunk, sizeof(chunk), &br) && br > 0) {}
            InternetCloseHandle(hUrl);
         }
         InternetCloseHandle(hNet);
      }
   } else {
      // Low-level path: InternetConnect + HttpOpenRequest (required to set method / headers).
      URL_COMPONENTSA uc = {};
      uc.dwStructSize = sizeof(uc);
      char hostBuf[256] = {}, pathBuf[2048] = {};
      uc.lpszHostName     = hostBuf; uc.dwHostNameLength = sizeof(hostBuf);
      uc.lpszUrlPath      = pathBuf; uc.dwUrlPathLength  = sizeof(pathBuf);

      if (InternetCrackUrlA(w->url, 0, 0, &uc)) {
         const BOOL  isHttps = (uc.nScheme == INTERNET_SCHEME_HTTPS);
         const DWORD flags   = INTERNET_FLAG_RELOAD |
                               (isHttps ? INTERNET_FLAG_SECURE |
                                          INTERNET_FLAG_IGNORE_CERT_CN_INVALID |
                                          INTERNET_FLAG_IGNORE_CERT_DATE_INVALID : 0);
         const INTERNET_PORT port = uc.nPort ? uc.nPort
                                  : (isHttps ? INTERNET_DEFAULT_HTTPS_PORT
                                             : INTERNET_DEFAULT_HTTP_PORT);

         HINTERNET hNet = http_open_session();
         if (hNet) {
            HINTERNET hConn = InternetConnectA(hNet, hostBuf, port,
                                               nullptr, nullptr, INTERNET_SERVICE_HTTP, 0, 0);
            if (hConn) {
               const char* path = pathBuf[0] ? pathBuf : "/";
               HINTERNET hReq = HttpOpenRequestA(hConn, w->method, path,
                                                 nullptr, nullptr, nullptr, flags, 0);
               if (hReq) {
                  DWORD hdrLen = w->headers ? (DWORD)strlen(w->headers) : 0;
                  if (HttpSendRequestA(hReq, w->headers, hdrLen,
                                       (LPVOID)w->body, w->body ? (DWORD)w->bodyLen : 0)) {
                     char chunk[4096]; DWORD br = 0;
                     while (InternetReadFile(hReq, chunk, sizeof(chunk), &br) && br > 0) {}
                  }
                  InternetCloseHandle(hReq);
               }
               InternetCloseHandle(hConn);
            }
            InternetCloseHandle(hNet);
         }
      }
   }

   http_async_free(w);
   return 0;
}

// Allocates a work item, copies all strings, spawns the thread, and detaches it.
// Returns true if the thread was created successfully.
static bool http_fire_and_forget(const char* method, const char* url,
                                 const char* body, size_t bodyLen,
                                 const char* headers)
{
   HttpAsyncWork* w = (HttpAsyncWork*)malloc(sizeof(HttpAsyncWork));
   if (!w) return false;
   memset(w, 0, sizeof(*w));

   w->url = _strdup(url);
   if (!w->url) { http_async_free(w); return false; }

   if (body && bodyLen > 0) {
      w->body = (char*)malloc(bodyLen);
      if (!w->body) { http_async_free(w); return false; }
      memcpy(w->body, body, bodyLen);
      w->bodyLen = bodyLen;
   }

   if (headers) {
      w->headers = _strdup(headers);
      if (!w->headers) { http_async_free(w); return false; }
   }

   strncpy_s(w->method, sizeof(w->method), method, _TRUNCATE);

   HANDLE hThread = CreateThread(nullptr, 0, http_async_worker, w, 0, nullptr);
   if (!hThread) { http_async_free(w); return false; }
   CloseHandle(hThread);  // detach — thread frees w on exit
   return true;
}

// HttpGetAsync(url) - fire-and-forget HTTP GET. Returns immediately.
static int lua_HttpGetAsync(lua_State* L)
{
   const char* url = g_lua.tolstring(L, 1, nullptr);
   if (url) http_fire_and_forget("GET", url, nullptr, 0, nullptr);
   return 0;
}

// HttpPutAsync(url, body) - fire-and-forget HTTP PUT. Returns immediately.
static int lua_HttpPutAsync(lua_State* L)
{
   const char* url = g_lua.tolstring(L, 1, nullptr);
   if (!url) return 0;
   size_t bodyLen = 0;
   const char* body = g_lua.tolstring(L, 2, &bodyLen);
   http_fire_and_forget("PUT", url, body, bodyLen, nullptr);
   return 0;
}

// HttpPostAsync(url, body) - fire-and-forget HTTP POST with Content-Type: application/json.
// Returns immediately. Ideal for Discord webhooks and other event notifications.
static int lua_HttpPostAsync(lua_State* L)
{
   const char* url = g_lua.tolstring(L, 1, nullptr);
   if (!url) return 0;
   size_t bodyLen = 0;
   const char* body = g_lua.tolstring(L, 2, &bodyLen);
   http_fire_and_forget("POST", url, body, bodyLen, "Content-Type: application/json\r\n");
   return 0;
}


// SetLoadDisplayLevel(path) - overrides the lvl the loading screen loads.
// The default is "Load\\load" (the vanilla load screen level).
// Call from Script root or ScriptPreInit.
//
// Paths resolve the same way ReadDataFile's do, minus the sublevel suffix:
//
//   SetLoadDisplayLevel("LOAD\\load.lvl")
//       -> data\_lvl_pc\LOAD\load.lvl
//   SetLoadDisplayLevel("dc:LOAD\\load.lvl")
//       -> <addon dir>\Data\_lvl_pc\LOAD\load.lvl  (e.g. addon\VTR\Data\_lvl_pc\...)
//   SetLoadDisplayLevel("..\\..\\addon\\VTR\\data\\_LVL_PC\\LOAD\\load")
//       -> raw path relative to data\_lvl_pc\, the original form, still supported
//
// The trailing ".lvl" is optional: LoadDisplay::LoadDataFile appends it via
// LoadUtil::MakeFullName, so it is stripped here and the engine puts it back.
//
// The resolution itself lives in lvl_resolve_data_path (core/lvl_read.hpp),
// shared with the LoadSoundLVL LoadConfig key so both accept the same forms.
//
// If the resolved file does not exist the call is rejected with a severity-3
// RedWarning and the previous load level is kept, so the modder gets a log line
// naming the path instead of a loading screen that silently renders nothing.
static int lua_SetLoadDisplayLevel(lua_State* L)
{
   const char* path = g_lua.tolstring(L, 1, nullptr);
   if (!path || !*path) return 0;

   char stem[260], reason[512];
   if (lvl_resolve_data_path(path, stem, sizeof(stem), nullptr, 0,
                             reason, sizeof(reason)) != LvlPathStatus::Ok) {
      warn_gamelog(RED_SEVERITY_ERROR, SRC_FILE, __LINE__,
         "SetLoadDisplayLevel(\"%s\"): %s Keeping \"%s\".\n",
         path, reason, g_loadDisplayPath);
      return 0;
   }

   strncpy_s(g_loadDisplayPath, sizeof(g_loadDisplayPath), stem, _TRUNCATE);
   return 0;
}

// SetInstanceProperty(name, property, value) - writes an [InstanceProperties] key
// on a named world object, the way the world layer would have at load time.
//
// Works on any named world object. Stock `SetProperty` resolves names through
// EntityEx::mIdMap_ and RTTI-checks against EntityEx, so a family deriving from
// Entity alone is invisible to it; those families (vehicle spawns so far) are
// handled explicitly, and every other name falls through to the same EntityEx
// path the stock callback uses. See entity/instance_props.cpp.
//
//   SetInstanceProperty("cp2_vspawn1", "ClassAllDEF", "cis_fly_droidfighter")
//   SetInstanceProperty("cp2_vspawn1", "SpawnTime", 15)
//
// The value is forwarded as a string because the engine's own property parsers
// take one and sscanf/atol it themselves, so numbers and booleans are stringified
// here to match what the ODF path would have handed them.
//
// Returns the number of objects written, or nil when the name matched nothing.
static int lua_SetInstanceProperty(lua_State* L)
{
   const char* name = g_lua.tolstring(L, 1, nullptr);
   const char* prop = g_lua.tolstring(L, 2, nullptr);
   if (!name || !*name || !prop || !*prop) return 0;

   // Lua 5.0's lua_tostring converts a number argument in place and hands back its
   // string form, so strings and numbers both come out of tolstring. Booleans do
   // not convert and come back null, so they are the only case to translate, and
   // they map to "1"/"0" the same way the engine's own Lua_Callbacks::SetProperty
   // maps them before calling a property parser.
   const char* value = g_lua.tolstring(L, 3, nullptr);
   if (!value) value = g_lua.toboolean(L, 3) ? "1" : "0";

   const int written = instance_props_set(name, prop, value);
   if (written == 0) return 0;

   g_lua.pushnumber(L, (float)written);
   return 1;
}

// SetFogRange(near, far) - sets the fog start/end distances.
// Both the D3D render state and the engine's internal copy are updated.
// Example: SetFogRange(0, 5) -- fog starts at 0m, fully opaque at 5m
static int lua_SetFogRange(lua_State* L)
{
   float fogNear = (float)g_lua.tonumber(L, 1);
   float fogFar  = (float)g_lua.tonumber(L, 2);

   const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
   auto res = [=](uintptr_t a) -> uintptr_t { return a - kUnrelocatedBase + base; };

   if (!g_addr->red_renderer_set_fog_range) return 0;

   // RedRenderer::SetFogRange(float, float) — cdecl, sets D3DRS_FOGSTART/FOGEND
   typedef void(__cdecl* SetFogRange_t)(float, float);
   ((SetFogRange_t)res(g_addr->red_renderer_set_fog_range))(fogNear, fogFar);

   // FLRenderer fog globals — persistence across RenderFarScene's save/restore.
   // (FLRenderer::SetFogRange only stores these; release builds inlined it.)
   if (g_addr->fl_fog_start && g_addr->fl_fog_end) {
      *(float*)res(g_addr->fl_fog_start) = fogNear;
      *(float*)res(g_addr->fl_fog_end)   = fogFar;
   }

   return 0;
}

// SetFogEnable(enable) - enables or disables fog rendering.
// Example: SetFogEnable(1) -- enable fog, SetFogEnable(0) -- disable
static int lua_SetFogEnable(lua_State* L)
{
   // Lua 5.0's tonumber reads a boolean as 0, so true/false go through toboolean.
   const bool enable = g_lua.isnumber(L, 1) ? g_lua.tonumber(L, 1) != 0
                                            : g_lua.toboolean(L, 1) != 0;

   const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
   auto res = [=](uintptr_t a) -> uintptr_t { return a - kUnrelocatedBase + base; };

   if (!g_addr->red_renderer_set_fog_enable) return 0;

   // RedRenderer::SetFogEnable(bool) — cdecl, sets D3DRS_FOGENABLE
   typedef void(__cdecl* SetFogEnable_t)(bool);
   ((SetFogEnable_t)res(g_addr->red_renderer_set_fog_enable))(enable);

   return 0;
}

// Replicates the post-create steps that VehicleSpawn::UpdateSpawn (0x00665a50)
// performs after EntityClass::Create. Stock `CreateEntity` Lua callback skips
// these, so vehicles spawned via it have no team / damage owner and weapons
// silently no-op.
//
//   1. ctrl = entity->vtable[9]()        — Entity::GetGameObject (null for a plain Entity)
//   2. ctrl->vtable[36](team)            — GameObject::SetTeam (+0x234 low 4 bits)
//   3. patch +0x234 bits 4-7  = team     — spawner-team
//   4. patch +0x234 bits 8-11 = team     — group/owner bits
//   5. ctrl->vtable[5]()                 — GameObject::Init
//
// Slots confirmed against the Phantom PDB vtables of Entity and GameObject.
// Returns true on success.
static bool apply_vehicle_fixup(void* entity, int team)
{
   if (!entity) return false;

   typedef void* (__fastcall* GetCtrl_t)(void* ecx);
   typedef void  (__fastcall* SetTeam_t)(void* ecx, void* edx, int team);
   typedef void  (__fastcall* Activate_t)(void* ecx);

   __try {
      void** evt = *(void***)entity;
      auto getCtrl = (GetCtrl_t)evt[9];
      void* ctrl = getCtrl(entity);
      if (!ctrl) return false;

      void** cvt = *(void***)ctrl;
      auto setTeam  = (SetTeam_t)cvt[36];
      auto activate = (Activate_t)cvt[5];

      setTeam(ctrl, nullptr, team);

      uint32_t* pField = (uint32_t*)((char*)ctrl + 0x234);
      uint32_t v = *pField;
      v = (v & ~0x0F0u) | ((uint32_t)(team & 0xF) << 4);
      v = (v & ~0xF00u) | ((uint32_t)(team & 0xF) << 8);
      *pField = v;

      activate(ctrl);
      return true;
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
   }
}

// Detour for stock `Lua_Callbacks::CreateEntity` (0x00472730).
//
// Original Lua signature: CreateEntity(className, matrix, name) -> entity|nil
// Extended signature:     CreateEntity(className, matrix, name [, team]) -> entity|nil
//
// After the original call returns (top of Lua stack = entity lightuserdata or
// nil), runs the vehicle fixup so weapons work. Only when a team is passed: the
// fixup re-inits any GameObject, not just vehicles, so a three-argument call
// stays exactly stock.
typedef int (__cdecl* fn_lua_create_entity_t)(void* L);
static fn_lua_create_entity_t original_lua_create_entity = nullptr;

static int __cdecl hooked_lua_create_entity(void* L)
{
   const bool haveTeam = g_lua.gettop && g_lua.isnumber &&
                         g_lua.gettop((lua_State*)L) >= 4 &&
                         g_lua.isnumber((lua_State*)L, 4);
   const int team = haveTeam ? g_lua.tointeger((lua_State*)L, 4) : 0;

   const int ret = original_lua_create_entity(L);
   if (ret <= 0 || !haveTeam) return ret;

   void* entity = g_lua.touserdata((lua_State*)L, -1);
   if (entity) apply_vehicle_fixup(entity, team);
   return ret;
}

void lua_create_entity_hook_install(uintptr_t exe_base)
{
   if (!g_addr->lua_create_entity) return;

   original_lua_create_entity = (fn_lua_create_entity_t)
      resolve(exe_base, g_addr->lua_create_entity);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_lua_create_entity, hooked_lua_create_entity);
   DetourTransactionCommit();
}

struct lua_func_entry {
   const char* name;
   lua_CFunction func;
};

// Dump the content budget on demand. Works on every build -- Lua is the mission
// scripting engine and is present everywhere, whereas the `~` console is
// modtools-only, so this is the only on-demand route that works on retail.
static int lua_ContentCensus(lua_State* L)
{
   (void)L;
   content_census_report();
   return 0;
}

// Whether version `have` is `want` or newer, part by part.
constexpr bool version_at_least(int haveMajor, int haveMinor, int havePatch,
                                int wantMajor, int wantMinor, int wantPatch)
{
   return haveMajor != wantMajor ? haveMajor > wantMajor
        : haveMinor != wantMinor ? haveMinor > wantMinor
        : havePatch >= wantPatch;
}
static_assert(version_at_least(1, 2, 0, 1, 2, 0) && version_at_least(1, 2, 0, 1, 1, 9) &&
              version_at_least(2, 0, 0, 1, 9, 9) && version_at_least(1, 10, 0, 1, 9, 0));
static_assert(!version_at_least(1, 2, 0, 1, 2, 1) && !version_at_least(1, 2, 0, 1, 3, 0) &&
              !version_at_least(1, 9, 9, 2, 0, 0) && !version_at_least(1, 1, 5, 1, 2, 0));

// GameExt.TestVersion(major [, minor [, patch]]): true if this BF2GameExt is that
// version or newer. A missing part counts as 0. Older builds have no field, so
// `GameExt and GameExt.TestVersion and GameExt.TestVersion(1, 1, 0)` is false there.
static int lua_TestVersion(lua_State* L)
{
   if (!g_lua.gettop || !g_lua.isnumber || !g_lua.tonumber || !g_lua.pushboolean) return 0;
   int want[3] = { 0, 0, 0 };
   const int count = g_lua.gettop(L);
   bool valid = count >= 1;
   for (int i = 0; valid && i < 3 && i < count; ++i) {
      if (g_lua.isnumber(L, i + 1)) want[i] = (int)g_lua.tonumber(L, i + 1);
      else valid = false;
   }
   static bool warned = false;
   if (!valid && !warned) {
      warn_gamelog(RED_SEVERITY_WARNING, SRC_FILE, __LINE__,
                   "GameExt.TestVersion takes version numbers, as in "
                   "GameExt.TestVersion(1, 1, 0); answering false.\n");
      warned = true;
   }
   g_lua.pushboolean(L, valid && version_at_least(GAMEEXT_VERSION_MAJOR, GAMEEXT_VERSION_MINOR,
                                                  GAMEEXT_VERSION_PATCH, want[0], want[1], want[2]));
   return 1;
}

// GameExt.IsPatchActive(section, key): true if that BF2GameExt.ini setting is on
// and running. A patch set that failed to install on this exe answers false.
static int lua_IsPatchActive(lua_State* L)
{
   if (!g_lua.gettop || !g_lua.tolstring || !g_lua.pushboolean) return 0;
   const bool two = g_lua.gettop(L) >= 2;
   const char* section = two ? g_lua.tolstring(L, 1, nullptr) : nullptr;
   const char* key     = two ? g_lua.tolstring(L, 2, nullptr) : nullptr;
   bool active = false;
   const bool known = section && key && ini_state_find(section, key, &active);
   static bool warned = false;
   if (!known && !warned) {
      warn_gamelog(RED_SEVERITY_WARNING, SRC_FILE, __LINE__,
                   "GameExt.IsPatchActive(\"%s\", \"%s\") names no BF2GameExt.ini setting; "
                   "it takes the section and key, as in "
                   "GameExt.IsPatchActive(\"Features\", \"Prone\"). Answering false.\n",
                   section ? section : "?", key ? key : "?");
      warned = true;
   }
   g_lua.pushboolean(L, active);
   return 1;
}

// ---------------------------------------------------------------------------
// Mission-script name validity
//
// GameLoop::SetNextMission is the only writer of mMissionScript, and the only
// caller that ever passes null is a path the shell does not take: MissionPlayList
// ::SelectNextEntry just returns false when the playlist runs out, leaving the
// buffer holding the last map. So the buffer alone cannot say whether a mission
// is live, and GetMissionName() would report a stale map to shell scripts forever.
//
// Two detours settle it. Entering the shell invalidates the name; setting a next
// mission validates it. The ordering is safe in every direction, because
// SetNextMission always runs after the last ShellState::Enter and before the
// mission's own Lua sees ScriptPreInit -- and the engine's own consumers
// (NetSetup::GetMapName, SetReinforcementCount, the playlist) never see a
// modified buffer, because this touches a DLL-side flag and not the engine's
// memory.
// ---------------------------------------------------------------------------

static bool s_missionScriptValid = false;

// modtools compiles SetNextMission __cdecl(char*); the retail LTCG builds made
// it __fastcall(char* ECX). Detours needs a trampoline matching each shape.
using fn_set_next_mission_cdecl    = void(__cdecl*)(const char*);
using fn_set_next_mission_fastcall = void(__fastcall*)(const char*, void*);
using fn_shell_state_enter         = void(__fastcall*)(void*, void*);

static fn_set_next_mission_cdecl    s_origSetNextMissionCdecl    = nullptr;
static fn_set_next_mission_fastcall s_origSetNextMissionFastcall = nullptr;
static fn_shell_state_enter         s_origShellStateEnter        = nullptr;

static void note_next_mission(const char* name)
{
   bool valid = false;
   __try {
      valid = (name != nullptr && name[0] != '\0');
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      valid = false;
   }
   s_missionScriptValid = valid;
}

static void __cdecl hooked_set_next_mission_cdecl(const char* name)
{
   s_origSetNextMissionCdecl(name);
   note_next_mission(name);
}

static void __fastcall hooked_set_next_mission_fastcall(const char* name, void* edx)
{
   s_origSetNextMissionFastcall(name, edx);
   note_next_mission(name);
}

static void __fastcall hooked_shell_state_enter(void* thisPtr, void* edx)
{
   // Invalidate before the original runs: nothing between here and the shell's
   // own scripts can set a new mission, and this way a shell script that runs
   // early in ShellLoop::Init already sees nil rather than the last map.
   s_missionScriptValid = false;
   s_origShellStateEnter(thisPtr, edx);
}

void script_name_mark_mission_started()
{
   // Safety net for the commandline-launch path (bxGameMain -> SetNextMission),
   // where a shell enter can land after the name was set.
   s_missionScriptValid = true;
}

void script_name_tracker_install(uintptr_t exe_base)
{
   if (!g_addr->game_loop_set_next_mission || !g_addr->gamestate_shell_state_enter)
      return;

   s_origShellStateEnter = (fn_shell_state_enter)
      resolve(exe_base, g_addr->gamestate_shell_state_enter);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   if (g_build == GameBuild::Modtools) {
      s_origSetNextMissionCdecl = (fn_set_next_mission_cdecl)
         resolve(exe_base, g_addr->game_loop_set_next_mission);
      DetourAttach(&(PVOID&)s_origSetNextMissionCdecl, hooked_set_next_mission_cdecl);
   } else {
      s_origSetNextMissionFastcall = (fn_set_next_mission_fastcall)
         resolve(exe_base, g_addr->game_loop_set_next_mission);
      DetourAttach(&(PVOID&)s_origSetNextMissionFastcall, hooked_set_next_mission_fastcall);
   }
   DetourAttach(&(PVOID&)s_origShellStateEnter, hooked_shell_state_enter);
   DetourTransactionCommit();
}

void script_name_tracker_uninstall()
{
   if (!s_origShellStateEnter) return;

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   if (s_origSetNextMissionCdecl)
      DetourDetach(&(PVOID&)s_origSetNextMissionCdecl, hooked_set_next_mission_cdecl);
   if (s_origSetNextMissionFastcall)
      DetourDetach(&(PVOID&)s_origSetNextMissionFastcall, hooked_set_next_mission_fastcall);
   DetourDetach(&(PVOID&)s_origShellStateEnter, hooked_shell_state_enter);
   DetourTransactionCommit();

   s_origSetNextMissionCdecl    = nullptr;
   s_origSetNextMissionFastcall = nullptr;
   s_origShellStateEnter        = nullptr;
}

// GetMissionName() - returns the mission-script name the match was launched
// from, e.g. "cor1l_con". Returns nil if the name is not available.
//
// Reads GameLoop::mMissionScript, the char[0x40] GameLoop::SetNextMission fills
// before the mission's Lua state is created, so it is already correct in
// ScriptPreInit and stays put for the whole match. The engine never exposed it:
// stock Lua has GetWorldFilename() for the .wld, but nothing for the script.
//
// Returns nil in the shell. The engine never clears mMissionScript when a match
// ends, so the raw buffer would keep naming the last map played; s_missionScriptValid
// tracks whether it still describes a live mission (see the detours below).
static int lua_GetMissionName(lua_State* L)
{
   if (!g_addr->game_loop_mission_script) { g_lua.pushnil(L); return 1; }

   // Back in the shell, or no mission ever set: no name to report.
   if (!s_missionScriptValid) { g_lua.pushnil(L); return 1; }

   const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
   const char* src = (const char*)(g_addr->game_loop_mission_script - kUnrelocatedBase + base);

   // Copy out under a bounded scan rather than pushing the engine buffer
   // directly: SetNextMission fills it with strncpy_safe(dst, src, 0x40), so a
   // name of exactly 0x40 chars would leave no terminator to stop at.
   char name[0x41];
   size_t len = 0;
   __try {
      while (len < 0x40 && src[len] != '\0') { name[len] = src[len]; ++len; }
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      len = 0;
   }
   name[len] = '\0';

   if (len == 0) { g_lua.pushnil(L); return 1; }   // shell before any map was set
   g_lua.pushlstring(L, name, len);
   return 1;
}

// GameExt_KeyDown(vk): physical state of a keyboard key (Win32 Virtual-Key code).
// Lets mission Lua read keys in-game (e.g. Ctrl + WASD) that stock Lua cannot see,
// enabling Helldivers-style stratagem input codes. Returns true if the key is down now.
static int lua_GameExt_KeyDown(lua_State* L)
{
   if (!g_lua.isnumber(L, 1)) { g_lua.pushboolean(L, 0); return 1; }
   const int vk = g_lua.tointeger(L, 1);
   int down = 0;
   if (vk > 0 && vk <= 0xFE)
      down = ((GetAsyncKeyState(vk) & 0x8000) != 0) ? 1 : 0;
   g_lua.pushboolean(L, down);
   return 1;
}

// Movement suppression for "stratagem input" mode. Lua toggles it; the dinput8
// proxy reads BF2GE_GetMoveSuppress() and masks movement keys from the game while on,
// so the character stops moving while GameExt_KeyDown still reads the raw keys.
static volatile int g_bf2ge_move_suppress = 0;
extern "C" __declspec(dllexport) int WINAPI BF2GE_GetMoveSuppress() { return g_bf2ge_move_suppress; }

static int lua_GameExt_SuppressMovement(lua_State* L)
{
   int on;
   if (g_lua.isnumber(L, 1)) on = (g_lua.tonumber(L, 1) != 0) ? 1 : 0;
   else                      on = g_lua.toboolean(L, 1) ? 1 : 0;
   g_bf2ge_move_suppress = on;
   return 0;
}

// -----------------------------------------------------------------------------
// GameExt_SetHudEvent(name, value [, type]) -- drive a HUD event by name so a
// .hud element bound to it reacts at runtime. This is the Helldivers stratagem
// arrow row's backbone: Lua lights and dims arrows as the WASD code is typed.
//
//   type (string, optional):
//     "bool"  (default) -- a notification; drives EventEnable / EventDisable.
//     "float"           -- drives EventAlpha / EventValue. value is a number.
//     "uint"            -- drives EventBitmap (a texture hash) and the like.
//     "color"           -- drives EventColor. value is a HEX STRING "RRGGBBAA"
//                          (NOT a number: this VM's lua_Number is a 32-bit float,
//                          so a packed colour would lose precision / overflow).
//                          type_Color (7) carries a POINTER to 4 colour bytes which
//                          the handler stores and derefs each frame, so we keep a
//                          persistent per-event buffer (hud_color_buffer) alive for
//                          the mission; re-sending the same name updates its colour.
//
// TIMING MATTERS. A .hud element's EventEnable binding only *finds* its
// EventClass at load time -- Steam's ReadEvent falls straight through on a null
// FindByHashID and never registers the handler (docs/RE/HUDEventBindings.md
// 1.3/1.4). So each event must EXIST before the .hud that binds it is read:
// call this once per event in ScriptInit, BEFORE the ReadDataFile of your .hud
// lvl (value can be 0), to create it; later calls just send to it. The first
// call runs EventClass::Create (valid any time after HUD::GameEvents::Open, which
// precedes ScriptInit); subsequent calls FindByHashID and Send.
//
// Returns true if sent, nil on a wrong build, outside a live HUD, or when the
// name already exists with a different event type.
namespace {

constexpr int      kHudTypeBool  = 1;      // HUD::EventClass::Type values
constexpr int      kHudTypeUint  = 3;
constexpr int      kHudTypeFloat = 4;
constexpr int      kHudTypeColor = 7;      // type_Color: mData is a RedColor* (4 bytes)
constexpr uint32_t kHudEC_Type   = 0x04;   // EventClass::mType offset

using HudFind     = void*(__cdecl*)(uint32_t hash);
using HudFindFast = void*(__fastcall*)(uint32_t hash);
using HudCreate   = void*(__cdecl*)(int type, const char* fmt, ...);
using HudSend     = void(__fastcall*)(void* ev, void* edx);

bool        s_hudEvTried  = false;
bool        s_hudEvOk     = false;
HudFind     s_hudFind     = nullptr;
HudFindFast s_hudFindFast = nullptr;
HudCreate   s_hudCreate   = nullptr;
HudSend     s_hudSend     = nullptr;

void hud_events_resolve()
{
   if (s_hudEvTried) return;
   s_hudEvTried = true;
   if (g_build != GameBuild::Modtools && g_build != GameBuild::Steam && g_build != GameBuild::GOG)
      return;
   if (!g_addr->hud_event_class_create || !g_addr->hud_event_class_find || !g_addr->hud_event_send)
      return;
   const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
   // FindByHashID: hash on the stack on modtools (cdecl), in ECX on retail (fastcall).
   void* find = resolve(base, g_addr->hud_event_class_find);
   if (g_build == GameBuild::Modtools) s_hudFind     = (HudFind)find;
   else                                s_hudFindFast = (HudFindFast)find;
   s_hudCreate = (HudCreate)resolve(base, g_addr->hud_event_class_create);
   s_hudSend   = (HudSend)  resolve(base, g_addr->hud_event_send);
   s_hudEvOk = s_hudCreate && s_hudSend && (s_hudFind || s_hudFindFast);
}

void* hud_find_event(uint32_t hash)
{
   if (s_hudFind)     return s_hudFind(hash);
   if (s_hudFindFast) return s_hudFindFast(hash);
   return nullptr;
}

// Find the EventClass for name, creating it with this type if absent. Returns
// null if it already exists with a different type (a send would be misread).
void* hud_find_or_create(int type, const char* name)
{
   void* cls = hud_find_event(pbl_hash(name));
   if (!cls) return s_hudCreate(type, "%s", name);   // literal name via the fmt
   if (*(const uint32_t*)((const uint8_t*)cls + kHudEC_Type) != (uint32_t)type)
      return nullptr;
   return cls;
}

void hud_send_event(void* cls, uint32_t data)
{
   if (!cls) return;
   struct { void* mClass; uint32_t mData; } ev = { cls, data };
   s_hudSend(&ev, nullptr);
}

// Persistent colour storage for type_Color events. The HUD::Event RedColor ctor
// stores the POINTER we pass and every EventColor handler dereferences it each
// frame, so the 4 bytes must outlive the send. One buffer per event name (keyed
// by its pbl hash); re-sending the same name rewrites its 4 bytes in place.
struct HudColorSlot { uint32_t hash; uint8_t rgba[4]; bool used; };
HudColorSlot s_hudColorSlots[32] = {};

uint8_t* hud_color_buffer(uint32_t hash)
{
   for (int i = 0; i < 32; ++i)
      if (s_hudColorSlots[i].used && s_hudColorSlots[i].hash == hash)
         return s_hudColorSlots[i].rgba;
   for (int i = 0; i < 32; ++i)
      if (!s_hudColorSlots[i].used) {
         s_hudColorSlots[i].used = true;
         s_hudColorSlots[i].hash = hash;
         return s_hudColorSlots[i].rgba;
      }
   return s_hudColorSlots[0].rgba;   // table full: reuse slot 0 rather than crash
}

} // namespace

static int lua_GameExt_SetHudEvent(lua_State* L)
{
   const char* name = g_lua.tolstring(L, 1, nullptr);
   if (!name || !*name) { g_lua.pushnil(L); return 1; }

   hud_events_resolve();
   if (!s_hudEvOk) { g_lua.pushnil(L); return 1; }

   const char* typeStr = g_lua.tolstring(L, 3, nullptr);
   int      type = kHudTypeBool;
   uint32_t data = 0;
   if (typeStr && (*typeStr == 'f' || *typeStr == 'F')) {
      type = kHudTypeFloat;
      const float v = g_lua.isnumber(L, 2) ? (float)g_lua.tonumber(L, 2) : 0.0f;
      memcpy(&data, &v, sizeof data);
   } else if (typeStr && (*typeStr == 'u' || *typeStr == 'U')) {
      type = kHudTypeUint;
      data = (uint32_t)(g_lua.isnumber(L, 2) ? g_lua.tointeger(L, 2) : 0);
   } else if (typeStr && (*typeStr == 'c' || *typeStr == 'C')) {
      // value is a colour as a HEX STRING "RRGGBBAA". It must NOT come through a
      // Lua number: this VM's lua_Number is a 32-bit float (lua_hooks.hpp), so a
      // packed colour >= 2^24 loses precision and >= 2^31 casts to 0x80000000.
      // Parse the 4 bytes here, stash them in a persistent per-event buffer and
      // carry its pointer as the event data (see note above).
      type = kHudTypeColor;
      const char* hex = g_lua.tolstring(L, 2, nullptr);
      uint32_t packed = 0;
      for (const char* p = hex; p && *p; ++p) {
         uint32_t d;
         const char c = *p;
         if      (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
         else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
         else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
         else continue;
         packed = (packed << 4) | d;
      }
      // RedColor is laid out B,G,R,A in memory (proven in game: a red authored as
      // RGBA bytes read back blue). Keep the intuitive RRGGBBAA input and store it
      // in the engine's BGRA byte order here.
      uint8_t* buf = hud_color_buffer(pbl_hash(name));
      buf[0] = (uint8_t)((packed >>  8) & 0xFF);   // B
      buf[1] = (uint8_t)((packed >> 16) & 0xFF);   // G
      buf[2] = (uint8_t)((packed >> 24) & 0xFF);   // R
      buf[3] = (uint8_t)( packed        & 0xFF);   // A
      data = (uint32_t)(uintptr_t)buf;
   } else {
      type = kHudTypeBool;   // notification; data is the flag the engine carries
      int on = 1;
      if (g_lua.isnumber(L, 2))      on = (g_lua.tonumber(L, 2) != 0) ? 1 : 0;
      else if (g_lua.gettop(L) >= 2) on = g_lua.toboolean(L, 2) ? 1 : 0;
      data = (uint32_t)on;
   }

   int ok = 0;
   __try {
      void* cls = hud_find_or_create(type, name);
      if (cls) { hud_send_event(cls, data); ok = 1; }
   } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }

   if (ok) g_lua.pushboolean(L, 1);
   else    g_lua.pushnil(L);
   return 1;
}

// ===========================================================================
// GameExt_OrbitalStrike(charIndex, radius) -- offensive stratagem. Kills every
// enemy unit within `radius` of the caster character (the one at charIndex).
// Returns the number killed, or nil on failure.
//
// Reuses the engine's own spatial query TeamManager::GetObjectsInRange and the
// entity offsets the aim-assist code already proved (controller/aim_assist.cpp):
//   entity = *(charSlot+0x148) - 0x240   (Controllable -> Entity base)
//   team   = *(entity+0x234) & 0xF       (0 = neutral)
//   health = *(float*)(entity+0x144)     (Damageable.mCurHealth)
//   pos    = collision-sphere position (TreeGridObject at entity+0x10/0x14/0x18)
// Lethal = write 0 to mCurHealth; the engine's Damageable update turns <=0 into
// a normal death next frame. A sanity band on the health float keeps us from
// writing into a non-damageable object the query may also return.
// ===========================================================================
namespace {

constexpr unsigned kOS_TeamAndType = 0x234;   // GameObject team (low nibble) + type bits
constexpr unsigned kOS_Health      = 0x144;   // Damageable.mCurHealth (float)
constexpr unsigned kOS_TgoStackPtr = 0x10;    // TreeGridObject.mStackPtr
constexpr unsigned kOS_TgoStackIdx = 0x14;    // TreeGridObject.mStackIdx
constexpr unsigned kOS_TgoInline   = 0x18;    // inline mCollisionSphere (Vec3 fallback)
constexpr unsigned kOS_CtrlToEnt   = 0x240;   // Entity* = Controllable* - 0x240
constexpr unsigned kOS_DamageableOff = 0x140; // Damageable sub-object = Entity + 0x140
constexpr unsigned kOS_KillSlot      = 1;     // Damageable vtable slot 1 = Kill() (carrier/walker kills)
// EntityProp world matrix. The modtools RE has it at CollisionObject+0xE4, but on
// the Steam build the struct is shifted +0xC: dumping +0xE4 put the world X at
// float index 15, i.e. the matrix really starts at +0xF0 and its translation
// (indices 12/13/14) lands at entity+0x120/0x124/0x128. Confirmed in game: the
// freshly-spawned beacon's +0xF0 translation matched os_entity_pos(beacon).
constexpr unsigned kOS_PropMatrix = 0xF0;  // Steam EntityProp mMatrix (modtools RE: +0xE4)

void os_entity_pos(uintptr_t entity, float out[3]);   // fwd decl (defined below)

// The CollisionObject sub-object sits at entity+0xC for these objects (both the
// player character and com_prop_buildzone): os_entity_pos reads the TreeGrid
// stack ptr at entity+0x10 == coll+0x4, and the matrix is at entity+0xF0 ==
// coll+0xE4. UpdateTreeGrid is a CollisionObject method, so it takes the coll.
constexpr unsigned kOS_CollObjOff = 0xC;

// CollisionObject::UpdateTreeGrid(this) -- __thiscall, no args. Re-inserts the
// object into the collision tree from its matrix (coll+0xE4). This is the second
// half of the engine's own teleport: PAAnimation::Update writes the matrix with
// SetMatrix(vtbl+0xC) and then calls this, unconditionally. Writing the matrix
// alone moves nothing visible; this makes the render/collision follow.
using fn_UpdateTreeGrid = void(__thiscall*)(void* collisionObject);
void os_update_tree_grid(void* entity)
{
   uintptr_t a = 0;
   switch (g_build) {
      case GameBuild::Steam: a = game_addrs::steam::update_tree_grid; break;
      default: a = 0;   // modtools/GOG address not mapped yet -> skip (no teleport)
   }
   if (!a || !entity) return;
   const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
   fn_UpdateTreeGrid fn = (fn_UpdateTreeGrid)((a - kUnrelocatedBase) + base);
   void* coll = (void*)((char*)entity + kOS_CollObjOff);
   __try { fn(coll); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Move an EntityProp to a world position by writing its mMatrix translation
// directly (matrix @ +0xF0, translation at indices 12/13/14). NO vtable call, so
// nothing can loop/hang. Self-validating guard: the write only happens when the
// current +0xF0 translation matches os_entity_pos(entity) -- the TreeGrid world
// position, a proven-correct source -- within a few units. That proves +0xF0 is
// really this object's matrix before we write; if it is not (another class, a bad
// pointer), the values disagree and we skip rather than corrupt memory.
// Returns true if it moved. (Render only; collision/TreeGrid unchanged.)
bool os_place_prop_at(void* entity, const float pos[3], const float nearRef[3])
{
   (void)nearRef;
   float* m = (float*)((char*)entity + kOS_PropMatrix);
   const auto fn_log = get_gamelog();

   float truth[3] = {0,0,0};
   bool gotTruth = false;
   __try { os_entity_pos((uintptr_t)entity, truth); gotTruth = true; }
   __except (EXCEPTION_EXECUTE_HANDLER) { gotTruth = false; }

   float tx, ty, tz;
   __try { tx = m[12]; ty = m[13]; tz = m[14]; }
   __except (EXCEPTION_EXECUTE_HANDLER) { if (fn_log) fn_log("[os_place_prop_at] read +0xF0 FAULT\n"); return false; }

   const float dx = tx - truth[0], dy = ty - truth[1], dz = tz - truth[2];
   const float d2 = dx*dx + dy*dy + dz*dz;
   if (fn_log) fn_log("[os_place_prop_at] ent=%p trans@0xF0=(%.1f,%.1f,%.1f) truth=(%.1f,%.1f,%.1f) d2=%.2f target=(%.1f,%.1f,%.1f)\n",
                      entity, tx,ty,tz, truth[0],truth[1],truth[2], d2, pos[0],pos[1],pos[2]);

   // +0xF0 must agree with the TreeGrid world pos (<= ~5u) to confirm it is the matrix.
   if (!gotTruth || !(d2 < 25.0f)) { if (fn_log) fn_log("[os_place_prop_at] SKIP: +0xF0 trans != world pos (not the matrix)\n"); return false; }

   __try { m[12] = pos[0]; m[13] = pos[1]; m[14] = pos[2]; }
   __except (EXCEPTION_EXECUTE_HANDLER) { if (fn_log) fn_log("[os_place_prop_at] write +0xF0 FAULT\n"); return false; }

   // Move the collision-sphere CENTRE cache too. UpdateTreeGrid re-inserts from
   // this slot (stackPtr+(idx+4)*16, exactly what os_entity_pos reads), NOT from
   // the matrix -- so without this it would just re-bucket the stale feet position.
   __try {
      uintptr_t stackPtr = *(uintptr_t*)((char*)entity + kOS_TgoStackPtr);
      if (stackPtr) {
         int idx = *(int*)((char*)entity + kOS_TgoStackIdx);
         float* sp = (float*)(stackPtr + (idx + 4) * 16);
         sp[0] = pos[0]; sp[1] = pos[1]; sp[2] = pos[2];
      } else {
         float* sp = (float*)((char*)entity + kOS_TgoInline);
         sp[0] = pos[0]; sp[1] = pos[1]; sp[2] = pos[2];
      }
   } __except (EXCEPTION_EXECUTE_HANDLER) { if (fn_log) fn_log("[os_place_prop_at] sphere write FAULT\n"); }

   // Teleport for real: re-insert into the collision tree from the new centre, so
   // the render/collision follow (the matrix write alone moves nothing visible).
   os_update_tree_grid(entity);
   if (fn_log) {
      float after[3] = {0,0,0};
      __try { os_entity_pos((uintptr_t)entity, after); } __except (EXCEPTION_EXECUTE_HANDLER) {}
      fn_log("[os_place_prop_at] after UpdateTreeGrid: os_entity_pos=(%.1f,%.1f,%.1f)\n", after[0],after[1],after[2]);
   }
   return true;
}

// Damageable::Kill — virtual slot 1, __thiscall(Damageable* this), no args. The
// engine's own death path (EntityCarrier::Kill / CommandWalker::Kill are this
// slot); it triggers a real death, unlike a passive write to mCurHealth.
using fn_DamageableKill = void(__thiscall*)(void* damageable);

using fn_TeamGetObjInRange = int(__cdecl*)(float* pos, float radius, uintptr_t* out,
                                           int maxCount, void* team, int flags, uintptr_t exclude);

// Steam/GOG build this with an LTCG convention (ECX=pos, EDX=out, XMM1=radius,
// stack=max,team,flags,exclude) -- not cdecl. Same marshalling thunk the aim
// assist uses (controller/aim_assist.cpp teamGetObjectsInRange_release_thunk).
void* s_os_torRelease = nullptr;
int __cdecl os_tor_release_thunk(float* pos, float radius, uintptr_t* out,
                                 int maxCount, void* team, int flags, uintptr_t exclude)
{
   int result;
   __asm {
      movss  xmm1, radius
      push   exclude
      push   flags
      push   team
      push   maxCount
      mov    edx, out
      mov    ecx, pos
      call   s_os_torRelease
      add    esp, 16
      mov    result, eax
   }
   return result;
}

fn_TeamGetObjInRange os_resolve_tor()
{
   const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
   uintptr_t addr = 0; bool release = false;
   switch (g_build) {
      case GameBuild::Modtools: addr = game_addrs::modtools::team_get_objects_in_range; break;
      case GameBuild::Steam:    addr = game_addrs::steam::team_get_objects_in_range;    release = true; break;
      case GameBuild::GOG:      addr = game_addrs::gog::team_get_objects_in_range;      release = true; break;
      default: return nullptr;
   }
   void* fn = (void*)((addr - kUnrelocatedBase) + base);
   if (release) { s_os_torRelease = fn; return os_tor_release_thunk; }
   return (fn_TeamGetObjInRange)fn;
}

void os_entity_pos(uintptr_t entity, float out[3])
{
   uintptr_t stackPtr = *(uintptr_t*)(entity + kOS_TgoStackPtr);
   if (stackPtr) {
      int idx = *(int*)(entity + kOS_TgoStackIdx);
      float* p = (float*)(stackPtr + (idx + 4) * 16);
      out[0] = p[0]; out[1] = p[1]; out[2] = p[2];
   } else {
      float* p = (float*)(entity + kOS_TgoInline);
      out[0] = p[0]; out[1] = p[1]; out[2] = p[2];
   }
}

// Controllable aim fields (same offsets the aim assist reads).
constexpr unsigned kOS_CtrlAimStart = 0xDC;   // PblVector3 eye position
constexpr unsigned kOS_CtrlAimDir   = 0xE8;   // PblVector3 aim direction

// Straight aim raycast: where the CROSSHAIR points. Reads eye+dir from the
// controllable, casts a straight ray (world mask 0x90, no soldiers) and returns
// the first world hit backed off `backoff` units toward the shooter so a surface
// hit sits in FRONT of the wall/ground instead of embedded in it. This is "where
// you aim" -- unlike the ballistic arc, which lobs up and lands higher. Returns
// true and fills `out`; false if the aim dir is unusable.
bool os_aim_land(uintptr_t ctrl, uintptr_t casterEnt, float backoff, float out[3])
{
   float eye[3], dir[3];
   eye[0]=*(float*)(ctrl+kOS_CtrlAimStart); eye[1]=*(float*)(ctrl+kOS_CtrlAimStart+4); eye[2]=*(float*)(ctrl+kOS_CtrlAimStart+8);
   dir[0]=*(float*)(ctrl+kOS_CtrlAimDir);   dir[1]=*(float*)(ctrl+kOS_CtrlAimDir+4);   dir[2]=*(float*)(ctrl+kOS_CtrlAimDir+8);
   float dl = sqrtf(dir[0]*dir[0]+dir[1]*dir[1]+dir[2]*dir[2]);
   if (dl < 1e-4f) return false;
   dir[0]/=dl; dir[1]/=dl; dir[2]/=dl;
   // Start 2u out so the ray clears the shooter's own body.
   float start[3] = { eye[0]+dir[0]*2.0f, eye[1]+dir[1]*2.0f, eye[2]+dir[2]*2.0f };
   const float kMaxDist = 500.0f;
   void* outHit = nullptr;
   void* exclude[1] = { (void*)casterEnt };
   float frac = engine_ray_hit(start, dir, kMaxDist, &outHit, exclude, 1, 0x90);
   float dist = (outHit && frac < 1.0f) ? frac * kMaxDist : kMaxDist;
   float hit = dist - backoff; if (hit < 0.0f) hit = 0.0f;
   out[0] = start[0] + dir[0]*hit;
   out[1] = start[1] + dir[1]*hit;
   out[2] = start[2] + dir[2]*hit;
   return true;
}

// Kill every enemy (team != 0, != excludeTeam) within `radius` of `pos`, using the
// engine spatial query + Damageable::Kill. Returns how many were killed.
int os_strike_at(fn_TeamGetObjInRange tor, const float pos[3], float radius,
                 uint32_t excludeTeam, uintptr_t excludeEnt)
{
   static uintptr_t nearby[128];
   float q[3] = { pos[0], pos[1], pos[2] };
   int n = tor(q, radius, nearby, 128, nullptr, 0x02, excludeEnt);
   if (n > 128) n = 128;
   int killed = 0;
   for (int i = 0; i < n; i++) {
      uintptr_t e = nearby[i];
      if (!e) continue;
      uint32_t t = (*(uint32_t*)(e + kOS_TeamAndType)) & 0xF;
      if (t == 0 || t == excludeTeam) continue;
      float h = *(float*)(e + kOS_Health);
      if (!(h > 0.0f && h < 1.0e6f)) continue;
      void*  dmg  = (void*)(e + kOS_DamageableOff);
      void** vtbl = *(void***)dmg;
      if (!vtbl) continue;
      fn_DamageableKill killFn = (fn_DamageableKill)vtbl[kOS_KillSlot];
      if (!killFn) continue;
      killFn(dmg);
      killed++;
   }
   return killed;
}

// March a ballistic arc from `start` along `vel` under gravity, raycasting each
// segment against the world; write the first impact (or the arc's end) to `land`.
void os_ballistic_land(const float start[3], const float vel0[3], uintptr_t excludeEnt,
                       float land[3])
{
   const float GRAV = -16.0f;   // world units/s^2 (tunable)
   const float DT   = 0.025f;
   const int   STEPS = 240;     // up to ~6 s of flight
   // Push the launch point ~2 units out along the throw so the first segment
   // starts OUTSIDE the caster's own collision; with mask 0x9A the old start
   // (right at the eye) hit the player's own body and "landed" at step 0.
   float vn = sqrtf(vel0[0]*vel0[0] + vel0[1]*vel0[1] + vel0[2]*vel0[2]);
   float p[3]  = { start[0], start[1], start[2] };
   if (vn > 1e-4f) {
      const float kNudge = 2.0f;
      p[0] += vel0[0]/vn*kNudge; p[1] += vel0[1]/vn*kNudge; p[2] += vel0[2]/vn*kNudge;
   }
   float v[3]  = { vel0[0], vel0[1], vel0[2] };
   void* exclude[1] = { (void*)excludeEnt };
   land[0] = p[0]; land[1] = p[1]; land[2] = p[2];
   for (int i = 0; i < STEPS; i++) {
      float np[3] = { p[0] + v[0]*DT, p[1] + v[1]*DT, p[2] + v[2]*DT };
      v[1] += GRAV * DT;
      float seg[3] = { np[0]-p[0], np[1]-p[1], np[2]-p[2] };
      float len = sqrtf(seg[0]*seg[0] + seg[1]*seg[1] + seg[2]*seg[2]);
      if (len > 1e-4f) {
         float dir[3] = { seg[0]/len, seg[1]/len, seg[2]/len };
         void* outHit = nullptr;
         // 0x90 = Ordnance::Update's world sweep (terrain+statics, NO soldiers)
         // so the arc lands on world geometry, never on a unit (incl. the caster).
         float frac = engine_ray_hit(p, dir, len, &outHit, exclude, 1, 0x90);
         if (outHit && frac < 1.0f) {               // world impact on this segment
            land[0] = p[0] + dir[0]*frac*len;
            land[1] = p[1] + dir[1]*frac*len;
            land[2] = p[2] + dir[2]*frac*len;
            return;
         }
      }
      p[0]=np[0]; p[1]=np[1]; p[2]=np[2];
      land[0]=p[0]; land[1]=p[1]; land[2]=p[2];
   }
}

} // namespace

static int lua_GameExt_OrbitalStrike(lua_State* L)
{
   if (!g_addr->char_array_base || !g_addr->max_chars) { g_lua.pushnil(L); return 1; }
   if (!g_lua.isnumber(L, 1) || !g_lua.isnumber(L, 2)) { g_lua.pushnil(L); return 1; }

   const int   charIndex = g_lua.tointeger(L, 1);
   const float radius    = (float)g_lua.tonumber(L, 2);
   if (radius <= 0.0f) { g_lua.pushnil(L); return 1; }

   fn_TeamGetObjInRange tor = os_resolve_tor();
   if (!tor) { g_lua.pushnil(L); return 1; }

   const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
   auto res = [=](uintptr_t a) -> uintptr_t { return a - kUnrelocatedBase + base; };
   const int maxChars = *(int*)res(g_addr->max_chars);
   if (charIndex < 0 || charIndex >= maxChars) { g_lua.pushnil(L); return 1; }
   const uintptr_t arrayBase = *(uintptr_t*)res(g_addr->char_array_base);
   if (!arrayBase) { g_lua.pushnil(L); return 1; }

   int killed = 0;
   __try {
      char* slot = (char*)arrayBase + charIndex * layout::Character::kSize;
      uintptr_t ctrl = *(uintptr_t*)(slot + layout::Character::kUnit);
      if (!ctrl) { g_lua.pushnil(L); return 1; }
      uintptr_t casterEnt  = ctrl - kOS_CtrlToEnt;
      uint32_t  casterTeam = (*(uint32_t*)(casterEnt + kOS_TeamAndType)) & 0xF;

      float pos[3];
      os_entity_pos(casterEnt, pos);
      killed = os_strike_at(tor, pos, radius, casterTeam, casterEnt);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      g_lua.pushnil(L); return 1;
   }

   g_lua.pushnumber(L, (float)killed);
   return 1;
}

// ===========================================================================
// GameExt_OrbitalStrikeAimed(charIndex, throwSpeed, radius) -- the stratagem
// "beacon": a ballistic arc is thrown from the caster's eye along the aim
// direction, raycast against the world to find where it LANDS, and the strike
// happens there (not at the caster's feet). Returns the number killed, or nil.
// This is the feasibility proof for the thrown-beacon system.
// ===========================================================================
static int lua_GameExt_OrbitalStrikeAimed(lua_State* L)
{
   if (!g_addr->char_array_base || !g_addr->max_chars) { g_lua.pushnil(L); return 1; }
   if (!g_lua.isnumber(L, 1) || !g_lua.isnumber(L, 2) || !g_lua.isnumber(L, 3)) { g_lua.pushnil(L); return 1; }

   const int   charIndex = g_lua.tointeger(L, 1);
   const float speed     = (float)g_lua.tonumber(L, 2);
   const float radius    = (float)g_lua.tonumber(L, 3);
   const auto  fn_GameLog = get_gamelog();
   if (fn_GameLog) fn_GameLog("[OrbitalStrikeAimed] called: char=%d speed=%.1f radius=%.1f\n",
                              charIndex, speed, radius);
   if (speed <= 0.0f || radius <= 0.0f) { if (fn_GameLog) fn_GameLog("[OrbitalStrikeAimed] bad speed/radius\n"); g_lua.pushnil(L); return 1; }

   fn_TeamGetObjInRange tor = os_resolve_tor();
   if (!tor) { if (fn_GameLog) fn_GameLog("[OrbitalStrikeAimed] tor resolve FAILED (build=%d)\n", (int)g_build); g_lua.pushnil(L); return 1; }

   const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
   auto res = [=](uintptr_t a) -> uintptr_t { return a - kUnrelocatedBase + base; };
   const int maxChars = *(int*)res(g_addr->max_chars);
   if (charIndex < 0 || charIndex >= maxChars) { g_lua.pushnil(L); return 1; }
   const uintptr_t arrayBase = *(uintptr_t*)res(g_addr->char_array_base);
   if (!arrayBase) { g_lua.pushnil(L); return 1; }

   int killed = 0;
   __try {
      char* slot = (char*)arrayBase + charIndex * layout::Character::kSize;
      uintptr_t ctrl = *(uintptr_t*)(slot + layout::Character::kUnit);
      if (!ctrl) { g_lua.pushnil(L); return 1; }
      uintptr_t casterEnt  = ctrl - kOS_CtrlToEnt;
      uint32_t  casterTeam = (*(uint32_t*)(casterEnt + kOS_TeamAndType)) & 0xF;
      (void)speed;   // strike at the straight aim point now, not a ballistic throw

      float land[3];
      if (!os_aim_land(ctrl, casterEnt, 2.0f, land)) { g_lua.pushnil(L); return 1; }

      killed = os_strike_at(tor, land, radius, casterTeam, casterEnt);
      if (fn_GameLog)
         fn_GameLog("[OrbitalStrikeAimed] team=%u land=(%.1f,%.1f,%.1f) killed=%d\n",
                    casterTeam, land[0],land[1],land[2], killed);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      if (fn_GameLog) fn_GameLog("[OrbitalStrikeAimed] EXCEPTION caught\n");
      g_lua.pushnil(L); return 1;
   }

   g_lua.pushnumber(L, (float)killed);
   return 1;
}

// ===========================================================================
// GameExt_PlaceBeaconAtAim(beaconEntity, charIndex, throwSpeed, radius)
//   beaconEntity: the userdata returned by Lua CreateEntity(...) for a visible
//   marker object. The DLL computes the ballistic landing from the caster's aim,
//   MOVES the beacon there (so you SEE where it lands), then strikes there.
// Returns killed count (nil on failure). The visible-beacon version of the
// thrown stratagem.
// ===========================================================================
static int lua_GameExt_PlaceBeaconAtAim(lua_State* L)
{
   const auto fn_GameLog = get_gamelog();
   if (fn_GameLog) fn_GameLog("[PlaceBeaconAtAim] called\n");
   if (!g_lua.touserdata) { if (fn_GameLog) fn_GameLog("[PlaceBeaconAtAim] NIL: touserdata unresolved\n"); g_lua.pushnil(L); return 1; }
   if (!g_addr->char_array_base || !g_addr->max_chars) { if (fn_GameLog) fn_GameLog("[PlaceBeaconAtAim] NIL: char array addr missing\n"); g_lua.pushnil(L); return 1; }
   if (!g_lua.isnumber(L, 2) || !g_lua.isnumber(L, 3) || !g_lua.isnumber(L, 4)) { if (fn_GameLog) fn_GameLog("[PlaceBeaconAtAim] NIL: args 2/3/4 not numbers\n"); g_lua.pushnil(L); return 1; }

   void* beacon        = g_lua.touserdata(L, 1);
   const int   charIndex = g_lua.tointeger(L, 2);
   const float speed     = (float)g_lua.tonumber(L, 3);
   const float radius    = (float)g_lua.tonumber(L, 4);
   if (fn_GameLog) fn_GameLog("[PlaceBeaconAtAim] char=%d speed=%.1f radius=%.1f beacon=%p\n", charIndex, speed, radius, beacon);
   if (speed <= 0.0f || radius <= 0.0f) { if (fn_GameLog) fn_GameLog("[PlaceBeaconAtAim] NIL: bad speed/radius\n"); g_lua.pushnil(L); return 1; }

   fn_TeamGetObjInRange tor = os_resolve_tor();
   if (!tor) { if (fn_GameLog) fn_GameLog("[PlaceBeaconAtAim] NIL: tor resolve failed (build=%d)\n", (int)g_build); g_lua.pushnil(L); return 1; }

   const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
   auto res = [=](uintptr_t a) -> uintptr_t { return a - kUnrelocatedBase + base; };
   const int maxChars = *(int*)res(g_addr->max_chars);
   if (charIndex < 0 || charIndex >= maxChars) { if (fn_GameLog) fn_GameLog("[PlaceBeaconAtAim] NIL: charIndex %d out of range (max %d)\n", charIndex, maxChars); g_lua.pushnil(L); return 1; }
   const uintptr_t arrayBase = *(uintptr_t*)res(g_addr->char_array_base);
   if (!arrayBase) { if (fn_GameLog) fn_GameLog("[PlaceBeaconAtAim] NIL: arrayBase null\n"); g_lua.pushnil(L); return 1; }

   int killed = 0;
   __try {
      char* slot = (char*)arrayBase + charIndex * layout::Character::kSize;
      uintptr_t ctrl = *(uintptr_t*)(slot + layout::Character::kUnit);
      if (!ctrl) { if (fn_GameLog) fn_GameLog("[PlaceBeaconAtAim] NIL: ctrl null (char has no unit)\n"); g_lua.pushnil(L); return 1; }
      uintptr_t casterEnt  = ctrl - kOS_CtrlToEnt;
      uint32_t  casterTeam = (*(uint32_t*)(casterEnt + kOS_TeamAndType)) & 0xF;

      float eye[3], dir[3];
      eye[0]=*(float*)(ctrl+kOS_CtrlAimStart); eye[1]=*(float*)(ctrl+kOS_CtrlAimStart+4); eye[2]=*(float*)(ctrl+kOS_CtrlAimStart+8);
      dir[0]=*(float*)(ctrl+kOS_CtrlAimDir);   dir[1]=*(float*)(ctrl+kOS_CtrlAimDir+4);   dir[2]=*(float*)(ctrl+kOS_CtrlAimDir+8);
      float dl = sqrtf(dir[0]*dir[0]+dir[1]*dir[1]+dir[2]*dir[2]);
      if (fn_GameLog) fn_GameLog("[PlaceBeaconAtAim] ctrl=%p eye=(%.1f,%.1f,%.1f) rawdir=(%.3f,%.3f,%.3f) dl=%.4f team=%u\n",
                                 (void*)ctrl, eye[0],eye[1],eye[2], dir[0],dir[1],dir[2], dl, casterTeam);
      if (dl < 1e-4f) { if (fn_GameLog) fn_GameLog("[PlaceBeaconAtAim] NIL: aim dir ~zero (bad offset 0xE8 on foot?)\n"); g_lua.pushnil(L); return 1; }
      dir[0]/=dl; dir[1]/=dl; dir[2]/=dl;

      float vel[3] = { dir[0]*speed, dir[1]*speed + 0.35f*speed, dir[2]*speed };
      float land[3];
      os_ballistic_land(eye, vel, casterEnt, land);

      float casterPos[3];
      os_entity_pos(casterEnt, casterPos);
      bool moved = false;
      // Lift the visible marker a few units off the ground so it is easy to spot
      // at the landing (the pad sits flush with / sunk into terrain at y~0). The
      // STRIKE still uses the true ground landing, not the lifted marker.
      const float kBeaconLift = 4.0f;
      float beaconPos[3] = { land[0], land[1] + kBeaconLift, land[2] };
      if (beacon) moved = os_place_prop_at(beacon, beaconPos, casterPos);  // VISIBLE marker at the landing
      killed = os_strike_at(tor, land, radius, casterTeam, casterEnt);
      if (fn_GameLog) fn_GameLog("[PlaceBeaconAtAim] eye=(%.1f,%.1f,%.1f) dir=(%.2f,%.2f,%.2f) "
                                 "vel=(%.1f,%.1f,%.1f) land=(%.1f,%.1f,%.1f) beacon=%p moved=%d killed=%d\n",
                                 eye[0],eye[1],eye[2], dir[0],dir[1],dir[2], vel[0],vel[1],vel[2],
                                 land[0],land[1],land[2], beacon, (int)moved, killed);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      if (fn_GameLog) fn_GameLog("[PlaceBeaconAtAim] EXCEPTION\n");
      g_lua.pushnil(L); return 1;
   }

   g_lua.pushnumber(L, (float)killed);
   return 1;
}

// ===========================================================================
// GameExt_RelocateMatrixToAim(matrix, charIndex, throwSpeed [, lift]) -> x,y,z | nil
//   matrix: a Lua matrix (e.g. from GetEntityMatrix) whose TRANSLATION this
//   overwrites with the ballistic landing of the caster's aim. The caller then
//   passes the SAME matrix to CreateEntity, so the prop is BORN at the landing
//   (its render node is baked at spawn; moving a prop after spawn does not move
//   its render -- proven: matrix+sphere+UpdateTreeGrid move collision only).
//   Returns the landing x,y,z, or nil on failure. (lift raises the spawn Y.)
// ===========================================================================
static int lua_GameExt_RelocateMatrixToAim(lua_State* L)
{
   const auto fn_GameLog = get_gamelog();
   if (!g_lua.touserdata) { g_lua.pushnil(L); return 1; }
   if (!g_addr->char_array_base || !g_addr->max_chars) { g_lua.pushnil(L); return 1; }
   if (!g_lua.isnumber(L, 2) || !g_lua.isnumber(L, 3)) { g_lua.pushnil(L); return 1; }

   void* mtxUser       = g_lua.touserdata(L, 1);
   const int   charIndex = g_lua.tointeger(L, 2);
   const float speed     = (float)g_lua.tonumber(L, 3);
   const float lift      = g_lua.isnumber(L, 4) ? (float)g_lua.tonumber(L, 4) : 0.0f;
   if (!mtxUser || speed <= 0.0f) { if (fn_GameLog) fn_GameLog("[RelocateMatrixToAim] NIL: bad matrix/speed\n"); g_lua.pushnil(L); return 1; }

   const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
   auto res = [=](uintptr_t a) -> uintptr_t { return a - kUnrelocatedBase + base; };
   const int maxChars = *(int*)res(g_addr->max_chars);
   if (charIndex < 0 || charIndex >= maxChars) { g_lua.pushnil(L); return 1; }
   const uintptr_t arrayBase = *(uintptr_t*)res(g_addr->char_array_base);
   if (!arrayBase) { g_lua.pushnil(L); return 1; }

   __try {
      char* slot = (char*)arrayBase + charIndex * layout::Character::kSize;
      uintptr_t ctrl = *(uintptr_t*)(slot + layout::Character::kUnit);
      if (!ctrl) { g_lua.pushnil(L); return 1; }
      uintptr_t casterEnt = ctrl - kOS_CtrlToEnt;
      (void)speed;   // landing is the straight aim point now, not a ballistic throw

      float land[3];
      if (!os_aim_land(ctrl, casterEnt, 2.0f, land)) { if (fn_GameLog) fn_GameLog("[RelocateMatrixToAim] NIL: aim dir ~zero\n"); g_lua.pushnil(L); return 1; }

      // PblMatrix is 0x40 bytes, row-major, translation row at +0x30 (float indices
      // 12/13/14). Validate the current translation is finite before writing.
      float* m = (float*)mtxUser;
      float cx = m[12], cy = m[13], cz = m[14];
      if (!(cx==cx && cy==cy && cz==cz)) { if (fn_GameLog) fn_GameLog("[RelocateMatrixToAim] NIL: matrix trans NaN\n"); g_lua.pushnil(L); return 1; }
      m[12] = land[0]; m[13] = land[1] + lift; m[14] = land[2];
      if (fn_GameLog) fn_GameLog("[RelocateMatrixToAim] was=(%.1f,%.1f,%.1f) -> land=(%.1f,%.1f,%.1f) lift=%.1f\n",
                                 cx,cy,cz, land[0],land[1],land[2], lift);
      g_lua.pushnumber(L, land[0]);
      g_lua.pushnumber(L, land[1]);
      g_lua.pushnumber(L, land[2]);
      return 3;
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      if (fn_GameLog) fn_GameLog("[RelocateMatrixToAim] EXCEPTION\n");
      g_lua.pushnil(L); return 1;
   }
}

// ===========================================================================
// Thrown-ball stratagem helpers (HD2 style: the ball the player throws is
// followed by the mission Lua until it settles; the strike happens there).
//
// GameExt_MatrixPos(matrix) -> x,y,z | nil
//   Reads the translation row (+0x30) of a Lua matrix (a copy from
//   GetEntityMatrix), so Lua can track a moving object's position.
// GameExt_SetMatrixPos(matrix, x, y, z) -> true | nil
//   Writes that translation, so Lua can spawn an upright prop (built from the
//   player's matrix) at a position read from another object.
// GameExt_OrbitalStrikeAt(x, y, z, radius, team [, charIndex]) -> killed | nil
//   Kills every damageable enemy (team != 0 and != `team`) within `radius` of
//   the point. charIndex, when its unit is alive, is excluded from the query.
// ===========================================================================
static int lua_GameExt_MatrixPos(lua_State* L)
{
   if (!g_lua.touserdata) { g_lua.pushnil(L); return 1; }
   const float* m = (const float*)g_lua.touserdata(L, 1);
   if (!m) { g_lua.pushnil(L); return 1; }
   __try {
      float x = m[12], y = m[13], z = m[14];
      if (!(x==x && y==y && z==z)) { g_lua.pushnil(L); return 1; }
      g_lua.pushnumber(L, x);
      g_lua.pushnumber(L, y);
      g_lua.pushnumber(L, z);
      return 3;
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      g_lua.pushnil(L); return 1;
   }
}

static int lua_GameExt_SetMatrixPos(lua_State* L)
{
   if (!g_lua.touserdata) { g_lua.pushnil(L); return 1; }
   float* m = (float*)g_lua.touserdata(L, 1);
   if (!m || !g_lua.isnumber(L, 2) || !g_lua.isnumber(L, 3) || !g_lua.isnumber(L, 4)) { g_lua.pushnil(L); return 1; }
   const float x = (float)g_lua.tonumber(L, 2);
   const float y = (float)g_lua.tonumber(L, 3);
   const float z = (float)g_lua.tonumber(L, 4);
   if (!(x==x && y==y && z==z)) { g_lua.pushnil(L); return 1; }
   __try {
      m[12] = x; m[13] = y; m[14] = z;
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      g_lua.pushnil(L); return 1;
   }
   g_lua.pushnumber(L, 1.0f);
   return 1;
}

static int lua_GameExt_OrbitalStrikeAt(lua_State* L)
{
   const auto fn_GameLog = get_gamelog();
   for (int i = 1; i <= 5; i++)
      if (!g_lua.isnumber(L, i)) { g_lua.pushnil(L); return 1; }
   float pos[3] = { (float)g_lua.tonumber(L, 1), (float)g_lua.tonumber(L, 2), (float)g_lua.tonumber(L, 3) };
   const float    radius = (float)g_lua.tonumber(L, 4);
   const uint32_t team   = (uint32_t)g_lua.tointeger(L, 5) & 0xF;
   if (!(pos[0]==pos[0] && pos[1]==pos[1] && pos[2]==pos[2]) || radius <= 0.0f) { g_lua.pushnil(L); return 1; }

   fn_TeamGetObjInRange tor = os_resolve_tor();
   if (!tor) { if (fn_GameLog) fn_GameLog("[OrbitalStrikeAt] tor resolve FAILED\n"); g_lua.pushnil(L); return 1; }

   int killed = 0;
   __try {
      uintptr_t casterEnt = 0;
      if (g_lua.isnumber(L, 6) && g_addr->char_array_base && g_addr->max_chars) {
         const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
         auto res = [=](uintptr_t a) -> uintptr_t { return a - kUnrelocatedBase + base; };
         const int charIndex = g_lua.tointeger(L, 6);
         const int maxChars  = *(int*)res(g_addr->max_chars);
         const uintptr_t arrayBase = *(uintptr_t*)res(g_addr->char_array_base);
         if (arrayBase && charIndex >= 0 && charIndex < maxChars) {
            char* slot = (char*)arrayBase + charIndex * layout::Character::kSize;
            uintptr_t ctrl = *(uintptr_t*)(slot + layout::Character::kUnit);
            if (ctrl) casterEnt = ctrl - kOS_CtrlToEnt;
         }
      }
      // Prefer the caster entity's own team (the path proven by OrbitalStrike);
      // the Lua-supplied team is only the fallback when the unit is gone.
      uint32_t useTeam = team;
      if (casterEnt) useTeam = (*(uint32_t*)(casterEnt + kOS_TeamAndType)) & 0xF;

      // Diagnostic census of what the query sees before striking.
      static uintptr_t probe[128];
      float q[3] = { pos[0], pos[1], pos[2] };
      int n = tor(q, radius, probe, 128, nullptr, 0x02, casterEnt);
      if (n > 128) n = 128;
      char teams[160]; int tl = 0; teams[0] = 0;
      for (int i = 0; i < n && tl < (int)sizeof(teams) - 8; i++) {
         uintptr_t e = probe[i];
         if (!e) continue;
         tl += _snprintf_s(teams + tl, sizeof(teams) - tl, _TRUNCATE, "%u:%.0f ",
                           (*(uint32_t*)(e + kOS_TeamAndType)) & 0xF, *(float*)(e + kOS_Health));
      }

      killed = os_strike_at(tor, pos, radius, useTeam, casterEnt);
      if (fn_GameLog)
         fn_GameLog("[OrbitalStrikeAt] pos=(%.1f,%.1f,%.1f) r=%.1f luaTeam=%u entTeam=%u caster=%p found=%d [%s] killed=%d\n",
                    pos[0], pos[1], pos[2], radius, team, useTeam, (void*)casterEnt, n, teams, killed);
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      if (fn_GameLog) fn_GameLog("[OrbitalStrikeAt] EXCEPTION caught\n");
      g_lua.pushnil(L); return 1;
   }
   g_lua.pushnumber(L, (float)killed);
   return 1;
}

static const lua_func_entry custom_functions[] = {
   { "GameExt_KeyDown",         lua_GameExt_KeyDown },
   { "GameExt_MatrixPos",       lua_GameExt_MatrixPos },
   { "GameExt_SetMatrixPos",    lua_GameExt_SetMatrixPos },
   { "GameExt_OrbitalStrikeAt", lua_GameExt_OrbitalStrikeAt },
   { "GameExt_RelocateMatrixToAim", lua_GameExt_RelocateMatrixToAim },
   { "GameExt_SuppressMovement", lua_GameExt_SuppressMovement },
   { "GameExt_SetHudEvent",     lua_GameExt_SetHudEvent },
   { "GameExt_OrbitalStrike",   lua_GameExt_OrbitalStrike },
   { "GameExt_OrbitalStrikeAimed", lua_GameExt_OrbitalStrikeAimed },
   { "GameExt_PlaceBeaconAtAim", lua_GameExt_PlaceBeaconAtAim },
   { "ContentCensus",         lua_ContentCensus },
   { "GetMissionName",        lua_GetMissionName },
   { "HttpGet",               lua_HttpGet },
   { "HttpPut",               lua_HttpPut },
   { "HttpPost",              lua_HttpPost },
   { "GetCharacterWeapon",    lua_GetCharacterWeapon },
   // v6 rebuild (2026-07-18): builds a real Weapon via WeaponClass::Build and
   // destroys the old one — the v1..v5 in-place mutation approach corrupted
   // MemoryPool free lists (see docs/RE/CharacterWeaponSystem.md).
   { "SetCharacterWeapon",    lua_SetCharacterWeapon },
   { "GetWeaponAmmo",         lua_GetWeaponAmmo },
   { "SetWeaponAmmo",         lua_SetWeaponAmmo },
   { "HttpGetAsync",          lua_HttpGetAsync },
   { "HttpPutAsync",          lua_HttpPutAsync },
   { "HttpPostAsync",         lua_HttpPostAsync },
   { "RemoveUnitClass",       lua_RemoveUnitClass },
   { "SetLoadDisplayLevel",      lua_SetLoadDisplayLevel },
   { "SetInstanceProperty",      lua_SetInstanceProperty },
   { "SetFogRange",              lua_SetFogRange },
   { "SetFogEnable",             lua_SetFogEnable },
   { nullptr, nullptr }
};

// True once register_lua_functions() has built the GameExt table. Until then
// _G.GameExt may be nil or (on the flat-global fallback) a boolean, so indexing
// it for the Disable field would be unsafe.
static bool s_gameExtTableBuilt = false;

bool gameext_is_disabled(lua_State* L)
{
   if (!s_gameExtTableBuilt) return false;

   // Must operate on the CALLER's live Lua state, not the cached global g_L:
   // during a C callback the engine may be running a different thread/state, and
   // pushing onto a stale state's stack faults and corrupts the VM.
   if (!L || !g_lua.pushlstring || !g_lua.gettable ||
       !g_lua.toboolean || !g_lua.gettop || !g_lua.settop)
      return false;

   bool disabled = false;
   const int top = g_lua.gettop(L);         // snapshot before we borrow the stack
   __try {
      g_lua.pushlstring(L, "GameExt", 7);
      g_lua.gettable(L, -10001);            // _G.GameExt (our table)
      g_lua.pushlstring(L, "disable", 7);
      g_lua.gettable(L, -2);                // GameExt.disable
      disabled = g_lua.toboolean(L, -1) != 0;
   } __except (EXCEPTION_EXECUTE_HANDLER) {
      disabled = false;
   }
   g_lua.settop(L, top);                     // always restore, even if a call faulted
   return disabled;
}

void register_lua_functions(lua_State* L)
{
   for (const lua_func_entry* entry = custom_functions; entry->name; ++entry)
      lua_register_func(L, entry->name, entry->func);

   // Detection table for scripts. GameExt is a table (still truthy, so the
   // classic `if GameExt then ... end` presence check keeps working) that
   // namespaces the metadata fields:
   //   if GameExt then ... end             -- extension present
   //   print(GameExt.version)              -- "1.0.0"
   //   if GameExt.build == "steam" then    -- which exe we're patching
   //   GameExt.disable = true              -- opt out of intrusive features
   //   if GameExt.TestVersion(1, 1, 0) then -- this version or newer
   //   GameExt.IsPatchActive("Features", "Prone") -- INI setting on and running
   const char* buildName = "unknown";
   switch (g_build) {
      case GameBuild::Modtools: buildName = "modtools"; break;
      case GameBuild::Steam:    buildName = "steam";    break;
      case GameBuild::GOG:      buildName = "gog";      break;
      default: break;
   }

   // IMPORTANT: this runs on every init_state, several times per mission load.
   // If _G.GameExt already exists (we built it on an earlier init_state for this
   // same Lua state), leave it completely alone — rebuilding it would wipe any
   // GameExt.disable a mission script has already set. On a genuinely new Lua
   // state (new mission) GameExt is nil again, so it rebuilds and disable resets.
   bool alreadyPresent = false;
   if (g_lua.pushlstring && g_lua.gettable && g_lua.toboolean &&
       g_lua.gettop && g_lua.settop) {
      const int top = g_lua.gettop(L);
      g_lua.pushlstring(L, "GameExt", 7);
      g_lua.gettable(L, -10001);                    // _G.GameExt (nil on a fresh state)
      alreadyPresent = g_lua.toboolean(L, -1) != 0; // any table is truthy
      g_lua.settop(L, top);
   }

   const bool canBuildTable = g_lua.newtable &&
                              g_lua.pushlstring && g_lua.settable;
   if (alreadyPresent) {
      s_gameExtTableBuilt = true;                    // table persists; reader can index it
   } else if (canBuildTable) {
      // Build _G.GameExt = { version = ..., build = ... }. Push the global key
      // first, then the table; set each field with settable(-3) (which leaves
      // the table on top), then settable(-10001) writes _G["GameExt"] = table.
      g_lua.pushlstring(L, "GameExt", 7);          // [K]
      if (g_lua.new_table(L)) {                     // [K, T]
         auto set_field = [&](const char* key, const char* value) {
            g_lua.pushlstring(L, key, strlen(key));
            g_lua.pushlstring(L, value, strlen(value));
            g_lua.settable(L, -3);
         };
         set_field("version", GAMEEXT_VERSION_STRING);
         set_field("build",   buildName);
         auto set_func = [&](const char* key, lua_CFunction func) {
            if (!g_lua.pushcclosure) return;
            g_lua.pushlstring(L, key, strlen(key));
            g_lua.pushcclosure(L, func, 0);
            g_lua.settable(L, -3);
         };
         set_func("TestVersion",   lua_TestVersion);
         set_func("IsPatchActive", lua_IsPatchActive);
         g_lua.settable(L, -10001);                 // _G.GameExt = T
         s_gameExtTableBuilt = true;                // enables gameext_is_disabled()
      } else if (g_lua.settop) {
         g_lua.settop(L, -2);                        // drop the dangling key
      }
   }

   // If the table couldn't be built (a build where Lua is wired but table
   // creation isn't — shouldn't happen in practice), fall back to a plain
   // truthy GameExt so `if GameExt then` still works. No flat GameExtVersion /
   // GameExtBuild globals: scripts read GameExt.version / GameExt.build.
   if (!s_gameExtTableBuilt)
      lua_set_global_bool(L, "GameExt", true);
}
