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

## Session Cheat Engine — AOB à trouver (prochaine session)

### Contexte
Le jeu est DS2 Scholar of the First Sin (Steam, x64, v1.02).
Le mod est chargé via `dinput8.dll` dans le dossier du jeu.
Les patterns trouvés vont dans `include/addresses.h`.

---

### 1. BonfireWarp — PRIORITÉ HAUTE
**Pourquoi** : permet le warp automatique cross-zone quand l'hôte se déplace.
Sans ça, `ExecuteBonfireWarp()` affiche juste une notification sans téléporter.

**Comment trouver :**
1. Ouvrir CE, attacher à `DarkSoulsII.exe`
2. Menu CE : `Memory View → Add address manually` → adresse = `GameManagerImp + 0x38` (PlayerData), puis `+ 0x16C` (LastBonfire)
3. Poser un **breakpoint on write** sur cette adresse (clic droit → `Find out what writes to this address`)
4. En jeu : aller à un feu de camp → choisir **Warp** vers une autre zone
5. CE s'arrête sur l'instruction qui écrit LastBonfire
6. Dans le call stack (bas de la fenêtre) remonter jusqu'à la fonction qui a initié le warp
7. Aller à cette fonction dans Memory View → noter les **8-16 premiers bytes du prologue**
8. Copier l'adresse → `exe+0xXXXXXX` (adresse relative à la base du module)

**Où mettre le résultat** dans `include/addresses.h` :
```cpp
constexpr AOBPattern BONFIRE_WARP = {
    "BonfireWarp",
    "\xXX\xXX\xXX...",   // bytes du prologue
    "xxxxxxxx...",        // masque (x = vérifié, ? = wildcard)
    0, 0
};
constexpr uint32_t WARP_MGR_FROM_GMI = 0xXX; // offset depuis GMI vers le warp manager
```

**Vérification** : `ExecuteBonfireWarp()` dans `progress_sync.cpp` scanne ce pattern au premier appel.

---

### 2. Matrice caméra (ViewProjection) — PRIORITÉ MOYENNE
**Pourquoi** : permet d'afficher les noms des joueurs au-dessus de leur personnage à l'écran (projection 3D → 2D).

**Comment trouver :**
1. Dans CE, chercher les floats de la matrice 4x4 (16 floats)
2. La matrice VP contient des valeurs comme `[0]` ≈ cot(FOV/2) / aspect_ratio, `[5]` ≈ cot(FOV/2)
3. Méthode : chercher la valeur `cot(45°)` ≈ `1.0` ou `cot(60°/2)` ≈ `1.732` dans les floats
4. Filtrer par "changed by code" pendant un mouvement de caméra
5. Trouver le pointeur statique qui pointe vers ce bloc de 64 bytes

**Alternative plus simple** : chercher dans les constant buffers DX11 via un outil comme RenderDoc (capturer une frame → chercher le cbuffer qui contient la VP matrix).

**Où mettre le résultat** dans `include/addresses.h` :
```cpp
constexpr AOBPattern CAMERA_VP_MATRIX = {
    "CameraVPMatrix",
    "\xXX\xXX...",
    "xxxx...",
    3, 7  // RIP-relative pointer
};
```

---

### 3. SpawnWorldItem — PRIORITÉ BASSE
**Pourquoi** : permet de synchroniser les drops d'items au sol (échange entre joueurs).

**Comment trouver :**
1. En jeu : équiper un item → le lâcher au sol (bouton drop dans l'inventaire)
2. Dans CE : poser un breakpoint sur les writes à la zone d'inventaire (PlayerData + 0x12EC)
3. Remonter le call stack jusqu'à la fonction qui crée l'entité world item
4. Son prologue = pattern à noter

**Note** : cette fonction est complexe (crée une entité physique dans le monde).
Vérifier la signature avant d'implémenter : elle prend probablement (WorldManager*, itemId, category, x, y, z, quantity).

---

### Workflow après avoir trouvé un pattern
1. Copier les bytes du prologue de la fonction (Memory View → sélectionner ~16 bytes → clic droit → Copy → Hex)
2. Renseigner dans `include/addresses.h` l'AOBPattern correspondant
3. Compiler (`cmake --build build --config Release`)
4. Tester en jeu
5. Committer avec `git commit -m "Add BonfireWarp AOB pattern (verified in-game)"`

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
