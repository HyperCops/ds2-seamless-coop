# Dark Souls 2: Seamless Co-op

Play through the entirety of Dark Souls 2: Scholar of the First Sin with friends. Boss kills, deaths, bonfires, area transitions — nothing disconnects you. One summon, the whole game.

> **Work in progress** — core systems implemented, actively being tested.

## Features

| Fonctionnalité | État |
|---|---|
| Session persistante (boss kills, morts, bonfires) | ✅ |
| Sync position / HP / stamina / level | ✅ 20 Hz / 2 Hz |
| Sync âmes (boss kills partagés) | ✅ |
| Sync boss kills (event flags) | ✅ |
| Sync items ramassés | ✅ |
| Sync zones / écrans de chargement | ✅ |
| HUD joueurs toujours visible (noms + barres HP) | ✅ |
| Codes de session (copier-coller, pas d'IP à taper) | ✅ |
| Téléportation d'urgence sur l'hôte | ✅ |
| Installeur graphique (détecte DS2 auto via Steam) | ✅ |
| Augmentation du cap joueurs (jusqu'à 6) | ✅ |
| Timer fantôme infini | ✅ |
| Warp automatique quand l'hôte change de zone | ⏳ (AOB manquant) |
| Noms 3D flottants | ❌ non prévu |
| Sync fog gates | ❌ non prévu |

## Installation

### Via l'installeur (recommandé)

1. Télécharge `DS2SeamlessCoopInstaller.exe` et `dinput8.dll` depuis les [Releases](../../releases/latest)
2. Lance `DS2SeamlessCoopInstaller.exe` — il détecte DS2 automatiquement
3. Clique **Installer le mod**, puis **Lancer DS2 via Steam**

### Manuellement

1. Copie `dinput8.dll` dans le dossier du jeu :
   ```
   Steam\steamapps\common\Dark Souls II Scholar of the First Sin\Game\
   ```
2. Lance DS2 normalement via Steam — le mod se charge automatiquement
3. Pour désinstaller : supprime `dinput8.dll` (l'installeur restaure le backup automatiquement)

## Comment jouer

1. Lance DS2, appuie sur **INSERT** pour ouvrir le menu co-op
2. **Hôte :** Tape un mot de passe → clique **Start Hosting** → copie le **code de session** (ex: `DS2-ODUu...`)
3. **Joueur :** Colle le code dans le champ **Session Code** → clique **Connect with Code**
4. C'est tout — les soapstones sont accordées automatiquement

## Connexion avec des amis

| Situation | Solution |
|---|---|
| Même réseau local | Code LAN affiché dans le menu hôte |
| Internet (port forwarding) | Code Internet — l'hôte ouvre le port **UDP 27015** |
| Sans port forwarding | [Hamachi](https://vpn.net) ou [ZeroTier](https://zerotier.com) → code LAN du réseau VPN |

Les IPs sont masquées par défaut (mode streamer). Le code de session encode l'IP — rien à taper manuellement.

## Comment ça marche

Le mod intercepte la couche protobuf du jeu pour bloquer les messages de déconnexion. Quand le jeu tente de terminer la session co-op (boss kill, mort, transition), le message est silencieusement supprimé. Technique identique à celle de [ds3os](https://github.com/TLeonardUK/ds3os) et du Seamless Co-op de LukeYui pour Elden Ring.

En plus du hook protobuf :
- **Hook SetEventFlag** : détecte les boss kills locaux et les broadcast aux pairs via UDP
- **AOB scan** : localise ItemGive, SetEventFlag, GameManagerImp sans offset hardcodé
- **Patches mémoire** : NOP des CALLs de renvoi de fantômes, augmentation du cap joueurs
- **dinput8 proxy** : chargement automatique par DS2 au démarrage, pas de launcher

## Compatibilité

- Dark Souls 2: Scholar of the First Sin (Steam, x64)
- Windows 10 / 11
- Ver 1.03, Calibrations 2.02

## Build depuis les sources

Prérequis : Visual Studio 2022 (workload "Desktop C++"), CMake 3.20+

```bash
git clone https://github.com/HyperCops/ds2-seamless-coop.git
cd ds2-seamless-coop
# Ouvrir dans Visual Studio → CMake détecté automatiquement
# Build → Build All
# Résultat dans dist/
```

## Tester sans second PC

Un client Python simule un second joueur en local :

```bash
python test_client.py --password test --name "MonPote"
# p = position  h = HP  b = boss  s = âmes  z = zone  q = quitter
```

## Crédits

- [ds3os](https://github.com/TLeonardUK/ds3os) — technique d'interception protobuf
- [LukeYui](https://github.com/LukeYui) — inspiration (Seamless Co-op DS1/DS3/ER)
- [Dear ImGui](https://github.com/ocornut/imgui) — UI overlay
- [MinHook](https://github.com/TsudaKagewortu/minhook) — function hooking

## Licence

MIT
