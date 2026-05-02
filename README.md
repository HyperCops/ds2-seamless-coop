# Dark Souls 2: Seamless Co-op

Mod pour Dark Souls 2: Scholar of the First Sin permettant de jouer en co-op persistant avec des amis. Basé sur l'interception protobuf (technique ds3os) et un serveur privé [ds3os](https://github.com/TLeonardUK/ds3os).

> **Work in progress** — connexion fonctionnelle, synchronisation en cours de développement.

---

## État actuel (honnête)

| Fonctionnalité | État | Notes |
|---|---|---|
| Connexion P2P (codes de session) | ✅ | Handshake chiffré AES-256-GCM |
| HUD joueurs (noms + barres HP) | ✅ | Toujours visible |
| Codes de session copier-coller | ✅ | Format `DS2-xxxx` |
| Augmentation cap joueurs (6 max) | ✅ | Patch mémoire runtime |
| Timer fantôme infini | ✅ | Patch mémoire runtime |
| Serveur privé (ds3os) | ✅ | Requis — voir configuration |
| Session persistante (boss kills) | ⚠️ | Nécessite ds3os actif côté hôte |
| Sync position / HP | ⚠️ | P2P uniquement, phantom natif DS2 |
| Sync boss kills (event flags) | ⚠️ | SetEventFlag non trouvé sur certains builds |
| Convocation directe sans signe | ❌ | En développement (nécessite RE Ghidra) |
| Apparence joueur solide (non fantôme) | ❌ | En développement (CharacterManager) |
| Drop d'items entre joueurs | ❌ | En développement |
| Warp automatique cross-zone | ❌ | AOB BonfireWarp non trouvé |

---

## Prérequis

- Dark Souls 2: Scholar of the First Sin (Steam, x64, v1.03)
- Windows 10 / 11
- [Hamachi](https://vpn.net) ou [ZeroTier](https://zerotier.com) (recommandé pour la connexion)

---

## Installation

### Hôte (celui qui héberge la partie)

1. Copie le contenu de `dist/host/` dans le dossier du jeu :
   ```
   Steam\steamapps\common\Dark Souls II Scholar of the First Sin\Game\
   ```
   Fichiers : `dinput8.dll`, `ds2_seamless_coop.ini`, `ds2_server_public.key`

2. **Avant de lancer DS2**, démarre le serveur :
   ```
   dist/host/StartServer.bat
   ```

3. Lance DS2 — le mod se charge automatiquement (title bar modifiée).

4. Appuie sur **INSERT** → **Start Hosting** → note le code de session.

### Joueur (celui qui rejoint)

1. Copie le contenu de `dist/joiner/` dans le dossier du jeu :
   ```
   Steam\steamapps\common\Dark Souls II Scholar of the First Sin\Game\
   ```

2. Dans `ds2_seamless_coop.ini`, change `server_ip` pour l'IP Hamachi de l'hôte.

3. Lance DS2 → **INSERT** → **Join** → colle le code de session → **Connect**.

4. Utilise ensuite la **White Sign Soapstone** pour te convoquer dans le monde de l'hôte.

> **Note :** La convocation native DS2 (pierre de savon) est encore requise pour apparaître en jeu. Une convocation directe sans signe est en développement.

---

## Comment ça marche

Le mod agit sur deux couches :

**1. Couche P2P (port UDP 27015)**
Connexion directe entre joueurs — synchronise position, HP, âmes, boss kills. Indépendant des serveurs FromSoftware.

**2. Couche session DS2 (ds3os)**
Le serveur privé ds3os remplace les serveurs FromSoftware. Il ne génère pas de message "retour du fantôme" lors des boss kills, ce qui maintient la session active.

Les hooks protobuf (technique ds3os) bloquent les messages de déconnexion côté client. Patches mémoire runtime : NOP des CALLs de renvoi de fantôme, augmentation du cap joueurs de 3 à 6.

---

## Configuration (`ds2_seamless_coop.ini`)

```ini
# Hôte
use_custom_server=true
server_ip=127.0.0.1    # 127.0.0.1 pour l'hôte, IP Hamachi de l'hôte pour le joueur

# Joueur
use_custom_server=true
server_ip=25.x.x.x     # IP Hamachi de l'hôte
```

---

## Tester sans second PC

```bash
python test_client.py --password test --name "MonPote"
# p = position  h = HP  b = boss  s = âmes  z = zone  q = quitter
```

---

## Build depuis les sources

Prérequis : Visual Studio 2022+ (workload "Desktop C++"), CMake 3.20+

```bash
git clone https://github.com/HyperCops/ds2-seamless-coop.git
cd ds2-seamless-coop
# Ouvrir dans Visual Studio → CMake détecté automatiquement
# Ou en ligne de commande (depuis un VS Developer Command Prompt) :
cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
# Résultat dans dist/
```

---

## Crédits

- [ds3os](https://github.com/TLeonardUK/ds3os) — technique d'interception protobuf + serveur privé
- [LukeYui](https://github.com/LukeYui) — inspiration (Seamless Co-op DS1/DS3/ER)
- [Dear ImGui](https://github.com/ocornut/imgui) — UI overlay
- [MinHook](https://github.com/TsudaKagewortu/minhook) — function hooking

## Licence

MIT
