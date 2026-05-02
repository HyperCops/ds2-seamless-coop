# DS2 Seamless Co-op — Contexte projet pour Claude

## Ce que c'est
Mod DLL pour Dark Souls 2: Scholar of the First Sin (Steam, x64) qui rend le co-op seamless comme les mods de Yui pour DS1/DS3/ER. Injecté via `dinput8.dll` proxy (chargé automatiquement par le jeu au démarrage). Pas de launcher requis.

---

## Architecture actuelle (Yui-style, pivot Mai 2026)

**Principe** : on hook la session native de DS2 — zéro transport custom. Le jeu gère lui-même les positions/animations via son propre networking Steam. On se contente de :
1. Garder la session vivante (bloquer les messages de déconnexion)
2. Découverte par mot de passe via Steam lobbies
3. *(à venir)* Appeler SpawnPhantom automatiquement quand un joueur rejoint

```
dinput8.dll  (proxy DirectInput, chargé par DS2 au boot)
    └── DllMain → SeamlessCoopMod::Initialize()
          1. MinHook init
          2. AOB scan (GameManagerImp, NetSessionManager)
          3. Protobuf hooks  ← bloque disconnect messages (mécanisme core)
          4. GameState::InstallHooks() (SetEventFlag, ItemGive)
          5. SessionManager + UI subsystems + update thread @20Hz
```

**Ce qui a été supprimé** (ancienne archi custom P2P) :
- `network_hooks.cpp` — WinsockHooks, ServerRedirect (supprimé du build)
- Transport custom : peer_manager, packet_handler, crypto, position sync via paquets
- Session codes (DS2-base64), champ IP, port, server_ip dans la config

---

## Fichiers clés

| Fichier | Rôle |
|---|---|
| `include/addresses.h` | Tous les AOB patterns + offsets mémoire DS2 |
| `include/hooks.h` | HookManager, ProtobufHooks, GameState |
| `include/session.h` | SessionManager (Steam lobbies async) |
| `include/ui.h` | Overlay (INSERT menu) + TitleScreenNotifier |
| `src/core/mod.cpp` | Init/Shutdown + LoadConfig |
| `src/hooks/game_state_hooks.cpp` | SetEventFlag hook, ItemGive hook |
| `src/session/session_manager.cpp` | Steam lobby create/join par mot de passe |
| `src/ui/overlay.cpp` | INSERT menu simplifié (Host / Join par password) |
| `third_party/steamworks/steam_minimal.h` | SDK Steam minimal (vtable manuelle) |

---

## Steam Lobbies — fonctionnement

- **Hôte** : `SteamMM::CreateLobby` → `SetLobbyData("ds2coop_pw", password)`
- **Rejoignant** : `AddRequestLobbyListStringFilter("ds2coop_pw", password, Equal)` → `RequestLobbyList` → `JoinLobby`
- Polling async dans `SessionManager::Update()` (pas de callbacks SteamAPI)
- `SteamAPI::RunCallbacks()` appelé à chaque Update

---

## Offsets mémoire vérifiés (Bob Edition v4.09.5 CT)

```
GameManagerImp (GMI) = AOB scan
GMI + 0x38  → PlayerData ptr
GMI + 0x60  → EventFlagManager ptr

PlayerData + 0x3C  → int32 HP (current)
PlayerData + 0x40  → int32 MaxHP
PlayerData + 0xF0  → int32 Souls
PlayerData + 0xF4  → uint32 SoulMemory
PlayerData + 0x16C → uint32 LastBonfire ID
PlayerData + 0xD0  → uint32 Level

NetSessionManager + 0x18 → SessionPtr
NetSessionManager + 0x20 → PlayerPtr
SessionPtr + 0x17C → float AllottedTime (phantom timer)
PlayerPtr  + 0x1F4 → uint32 phantom field (0 = host perms)
```

---

## AOB Patterns

| Pattern | Statut |
|---|---|
| `GameManagerImp` | ✅ Vérifié |
| `NetSessionManager` | ✅ Vérifié |
| `ProtobufSerialize` | ✅ Vérifié (ds3os) |
| `ProtobufParse` | ✅ Vérifié (ds3os) |
| `SetEventFlag` | ✅ Pattern écrit, à vérifier en jeu |
| `ItemGive` | ✅ Pattern (DS2S-META), à vérifier en jeu |
| `BonfireWarp` | ❌ Pattern vide — TODO CE |
| `SpawnPhantom` | ❌ Pas encore trouvé — voir section CE ci-dessous |

---

## CE Findings — session 2026-05-02

### Adresses trouvées

| Adresse | Rôle |
|---|---|
| `DarkSoulsII.exe+0x51B272` | Instruction qui charge le NetSessionManager (MOV RCX,[RIP+x]) |
| `DarkSoulsII.exe+0x2C71B0` | Thunk vtable appelé depuis le gestionnaire de session |
| `DarkSoulsII.exe+0x16A4865` | **Slot fantôme flag 0** : 0→1 au spawn, 1→0 au départ |
| `DarkSoulsII.exe+0x16A4899` | **Slot fantôme flag 1** : idem (slot 2 ou champ lié) |

### Thunk à +0x2C71B0 (décodé)
```asm
push rdi
sub rsp, 20h
mov rdi, rcx          ; save 'this'
mov rcx, [rcx+0x40]   ; charge sous-objet à offset 0x40
test rcx, rcx
jz +F                 ; null check
mov rax, [rcx]        ; vtable
add rsp, 20h
pop rdi
jmp [rax+0x90]        ; → vtable slot 18
```
= dispatche `this->field_0x40->vtable[18]()`

### SpawnPhantom — à trouver

**Méthode** (avec un ami ou PNJ invocable) :
1. CE → First Scan, Value Type = `Byte`, valeur = `0` (fantôme absent)
2. Invoquer le fantôme → Next Scan valeur = `1`
3. Répéter 2-3 cycles → isoler `+0x16A4865` et `+0x16A4899`
4. Clic droit → **"Find out what ACCESSES this address"** (VEH Debugger activé)
5. Invoquer à nouveau → CE montre l'instruction → remonter au début de la fonction
6. Noter les 16 premiers bytes du prologue → c'est SpawnPhantom

**Une fois trouvé** : appeler depuis `SessionManager::Update()` quand `m_state` passe à `Connected`.

---

## Fonctionnalités — état actuel

### ✅ Implémenté
- Protobuf hooks (bloc disconnect → session persistante à travers morts/boss/warp)
- Steam lobby discovery par mot de passe (CreateLobby / RequestLobbyList / JoinLobby)
- SetEventFlag hook (boss kills détectés localement)
- ItemGive hook
- Player HUD 2D (noms + HP bars, style Yui)
- Overlay simplifié (INSERT → Host/Join par password, pas d'IP)
- Config INI minimale (session_password, sync_bonfires, etc.)

### ❌ Pas encore fait
- **SpawnPhantom** : auto-invoquer sans soapstone quand lobby rejoint ← PRIORITÉ #1
- **BonfireWarp AOB** : warp cross-zone automatique
- Nettoyer peer_manager / packet_handler / crypto du build (supprimés de la logique mais fichiers encore présents)

### ⏳ Décidé de ne pas faire
- Transport custom (positions, HP via paquets) — le jeu gère ça nativement
- Fog gate sync

---

## Patches mémoire appliqués au démarrage

```cpp
// player_sync.cpp::Initialize()
PatchPhantomDismissalLoops()  // NOP 2 CALLs dans FUN_140191bb0
                               // exe+0x191c87 et exe+0x191d17
                               // Évite que les boss kills renvoient les phantoms

PatchPlayerCap()               // exe+0x6ab0b9 : 0x03 → max_players (config)
                               // exe+0x6ab15e : pareil dans le protobuf
```

---

## Règles de code du projet

- Namespace principal : `DS2Coop::`
- Logs : `LOG_INFO(...)` / `LOG_DEBUG(...)` / `LOG_WARNING(...)` / `LOG_ERROR(...)`
- Memory reads/writes : `Memory::Read<T>(addr, &val)` / `Memory::Write<T>(addr, val)`
- Hooks via MinHook : `HookManager::GetInstance().InstallHook(target, detour, &original)`
- AOB scans : `PatternScanner::FindPattern(pattern, mask, nullptr)`
- `__try/__except` : ne pas mettre dans des fonctions avec objets C++ (C2712 MSVC)
- Steam SDK : vtable manuelle via `SteamVCall<Ret>(iface, slot, args...)` (SDK 2013)

---

## Config INI (dist/)

```ini
enabled=true
debug_logging=false
max_players=6
session_password=coop
allow_invasions=false
sync_bonfires=true
sync_items=false
sync_enemies=false
```

Même fichier pour hôte ET rejoignant. Pas d'IP, pas de port, pas de serveur.
