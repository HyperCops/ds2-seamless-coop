# DS2 Seamless Co-op — Contexte projet pour Claude

## Ce que c'est
Mod DLL pour Dark Souls 2: Scholar of the First Sin (Steam, x64) qui rend le co-op seamless comme les mods de Yui pour DS1/DS3/ER. Injecté via `dinput8.dll` proxy (chargé automatiquement par le jeu au démarrage). Pas de launcher requis.

---

## Architecture en un coup d'œil

```
dinput8.dll  (proxy DirectInput, chargé par DS2 au boot)
    └── DllMain → SeamlessCoopMod::Initialize()
          1. MinHook init
          2. AOB scan (GameManagerImp, NetSessionManager)
          3. Protobuf hooks  ← bloc les disconnect messages (mécanisme core)
          4. Winsock hooks + server redirect
          5. PlayerSync::Initialize() (AOB ItemGive) → GameState::InstallHooks()
          6. Network / Session / UI subsystems + update thread @20Hz
```

---

## Fichiers clés — ce qu'ils font

| Fichier | Rôle |
|---|---|
| `include/addresses.h` | Tous les AOB patterns + offsets mémoire DS2 |
| `include/network.h` | PacketType enum + structs de paquets |
| `include/sync.h` | Déclarations PlayerSync / ProgressSync |
| `include/hooks.h` | HookManager, ProtobufHooks, GameState |
| `include/ui.h` | Overlay (INSERT menu) + TitleScreenNotifier |
| `src/core/mod.cpp` | Init/Shutdown + LoadConfig |
| `src/hooks/game_state_hooks.cpp` | SetEventFlag hook, ItemGive hook, HookManager impl |
| `src/sync/player_sync.cpp` | Position/HP/souls sync, zone transition detection, TeleportToHost |
| `src/sync/progress_sync.cpp` | Boss kills, event flags, souls, items, zone transition broadcast |
| `src/network/packet_handler.cpp` | Routing des paquets reçus → subsystems |
| `src/ui/overlay.cpp` | INSERT menu, session codes, player HUD (toujours visible) |
| `launcher/installer_gui.cpp` | Installeur GUI Win32 (détecte DS2, copie DLL) |

---

## Offsets mémoire vérifiés (Bob Edition v4.09.5 CT)

```
GameManagerImp (GMI) = AOB scan
GMI + 0x38  → PlayerData ptr
GMI + 0x60  → EventFlagManager ptr
GMI + 0xD0  → PlayerCtrl ptr
GMI + 0xA8  → GameDataManager ptr (nom du personnage)

PlayerData + 0x30  → float X
PlayerData + 0x34  → float Y
PlayerData + 0x38  → float Z
PlayerData + 0x3C  → int32 HP (current)
PlayerData + 0x40  → int32 MaxHP
PlayerData + 0xF0  → int32 Souls (dépensables)
PlayerData + 0xF4  → uint32 SoulMemory
PlayerData + 0x16C → uint32 LastBonfire ID
PlayerData + 0xD0  → uint32 Level

PlayerCtrl + 0x168 → int32 HP (current) — source fiable
PlayerCtrl + 0x170 → int32 MaxHP        — source fiable

EventFlagManager + 0x20 → uint32[] bitfield
  flag bit = bitfield[flagId >> 5] & (1 << (flagId & 31))

NetSessionManager + 0x18 → SessionPtr
NetSessionManager + 0x20 → PlayerPtr
SessionPtr + 0x17C → float AllottedTime (phantom timer)
PlayerPtr  + 0x1F4 → uint32 phantom field (0 = host perms)
```

---

## AOB Patterns (dans addresses.h)

| Pattern | Statut |
|---|---|
| `GameManagerImp` | ✅ Vérifié |
| `NetSessionManager` | ✅ Vérifié |
| `ProtobufSerialize` | ✅ Vérifié (ds3os) |
| `ProtobufParse` | ✅ Vérifié (ds3os) |
| `SetEventFlag` | ✅ Pattern écrit, à vérifier en jeu |
| `ItemGive` | ✅ Pattern (DS2S-META), à vérifier en jeu |
| `BonfireWarp` | ❌ Pattern vide — TODO Cheat Engine |

---

## Paquets réseau (PacketType enum)

```
0x01 Handshake       0x20 PlayerPosition   0x30 BossDefeated
0x02 Disconnect      0x21 PlayerAction     0x31 BonfireRest
0x03 Heartbeat       0x22 PlayerState      0x33 ItemPickup
0x10 SessionCreate   0x23 PlayerDeath      0x34 EventFlag
0x11 SessionJoin     0x24 PlayerRespawn    0x35 SoulsGranted
0x12 SessionLeave                          0x36 ZoneTransition
```

PacketHeader (21 bytes packed) : `uint32 magic(DS2C) | uint8 type | uint32 size | uint32 seq | uint64 ts`

---

## Fonctionnalités — état actuel

### ✅ Implémenté
- Protobuf hooks (bloc disconnect → session persistante)
- Position sync @20Hz
- HP/stamina/level sync @2Hz
- Souls sync (delta, seuil=1, toute augmentation partagée)
- Boss kill sync (SetEventFlag hook → BossDefeatedPacket → ApplyEventFlagToMemory)
- Item pickup sync (ItemGive hook → ItemPickupPacket)
- Bonfire sync (état "allumé" écrit dans la save des pairs)
- Zone transition detection (LastBonfire + position jump > 50u)
- ZoneTransitionPacket broadcast + ExecuteBonfireWarp (fallback notification)
- Emergency teleport (TeleportToHost, cooldown 10s)
- Player HUD 2D toujours visible (noms + HP bars, style Yui)
- Session codes (DS2-<base64> encodant IP:port:password)
- Installeur GUI Win32 (détection Steam registry, backup vanilla DLL)
- Config INI (enabled, max_players, port, sync_*, server_*)

### ❌ Pas encore fait
- BonfireWarp AOB (à trouver avec CE pour le zone sync automatique)
- Emergency teleport cross-zone (actuellement same-zone only)
- Noms de personnages au-dessus des joueurs en 3D world space

### ⏳ Décidé de ne pas faire
- Fog gate sync (chaque joueur passe quand il veut)

---

## Globals cross-TU importants

```cpp
// player_sync.cpp — extern dans game_state_hooks.cpp et progress_sync.cpp
void* g_itemGiveFunc    = nullptr;  // void* pour éviter type mismatch cross-TU
bool  g_itemGiveScanned = false;
bool  g_ourItemGiveCall = false;    // guard anti-reboucle ItemGive hook

// progress_sync.cpp — interne
static void* g_bonfireWarpFunc    = nullptr;
static bool  g_bonfireWarpScanned = false;
```

---

## Patches mémoire appliqués au démarrage

```cpp
// player_sync.cpp::Initialize()
PatchPhantomDismissalLoops()  // NOP 2 CALLs dans FUN_140191bb0
                               // exe+0x191c87 et exe+0x191d17
                               // Evite que les boss kills renvoient les phantoms

PatchPlayerCap()               // exe+0x6ab0b9 : 0x03 → max_players (config)
                               // exe+0x6ab15e : pareil dans le protobuf
```

---

## Session codes (overlay)

Format : `DS2-` + Base64(`"IP:PORT:password"`)
Exemple : `DS2-ODUuMjMuMTE0Ljc6MjcwMTU6Y29vcA==`

- L'hôte voit ses codes dès qu'il tape un mot de passe (avant même de cliquer Start Hosting)
- Le guest colle le code dans Join → champ "Session Code" → Connect with Code
- Fallback : champs IP + password manuels toujours disponibles

---

## Erreurs de compilation connues (non corrigées à ce jour)

### game_state_hooks.cpp
- **C2712** ligne ~72 : `__try` dans fonction avec objets C++ → isoler dans sous-fonction SEH pure
- **C2653/C2065** ligne ~170 : `Network::ItemPickupPacket` — manque `#include "../../include/network.h"`

### progress_sync.cpp  
- **C2712** ligne ~441 : même problème `__try` + objets C++

### player_sync.cpp
- **C2065** lignes 279-280-286 : `g_itemGiveScanned` / `g_itemGiveFunc` utilisés avant leur définition → ajouter déclarations forward en haut du fichier

---

## Règles de code du projet

- Namespace principal : `DS2Coop::`
- Logs : `LOG_INFO(...)` / `LOG_DEBUG(...)` / `LOG_WARNING(...)` / `LOG_ERROR(...)`
- Memory reads/writes : `Memory::Read<T>(addr, &val)` / `Memory::Write<T>(addr, val)`
- Hooks via MinHook : `HookManager::GetInstance().InstallHook(target, detour, &original)`
- AOB scans : `PatternScanner::FindPattern(pattern, mask, nullptr)`
- Paquets : toujours `header.magic = 0x44533243` ('DS2C')
- `__try/__except` : **ne pas mettre dans des fonctions qui ont des objets C++ avec destructeurs** (C2712 MSVC)
- Globals cross-TU : utiliser `void*` plutôt que des typedefs de pointeurs de fonctions pour éviter les incompatibilités de types

---

## Fichiers de test

```
DS2_Seamless_Coop/test_client.py       — faux client Python (simule un 2e joueur en local)
DS2_Seamless_Coop/DS2CoopInstaller_Preview.ps1  — aperçu PS1 de l'installeur GUI
```

Usage test client :
```
python test_client.py --password test --name "MonPote"
# Commandes : p=position, h=HP, b=boss, s=souls, z=zone, d=death, q=quit
```
