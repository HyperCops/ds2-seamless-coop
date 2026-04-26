"""
DS2 Seamless Co-op — Faux client de test
=========================================
Simule un second joueur qui se connecte au mod en local (127.0.0.1:27015).

Ce script permet de tester SANS un second PC / une seconde copie de DS2 :
  - Connexion + handshake
  - Apparition du joueur dans le HUD (noms + barre HP)
  - Sync de position (déplacement simulé)
  - Sync d'état (HP, âmes)
  - Déconnexion propre

Usage :
  python test_client.py [--host 127.0.0.1] [--port 27015] [--password tonmdp] [--name "Test Joueur"]

Prérequis : Python 3.8+, aucune dépendance externe.
"""

import socket
import struct
import time
import math
import argparse
import threading
import sys

# ── Packet types (miroir de network.h) ────────────────────────────────────────
PT_HANDSHAKE      = 0x01
PT_DISCONNECT     = 0x02
PT_HEARTBEAT      = 0x03
PT_PLAYER_POS     = 0x20
PT_PLAYER_STATE   = 0x22
PT_BOSS_DEFEATED  = 0x30
PT_BONFIRE_REST   = 0x31
PT_ITEM_PICKUP    = 0x33
PT_SOULS_GRANTED  = 0x35
PT_ZONE_TRANSIT   = 0x36

MAGIC = 0x44533243  # 'DS2C'

# ── Struct helpers ─────────────────────────────────────────────────────────────
# PacketHeader : uint32 magic | uint8 type | uint32 size | uint32 seq | uint64 ts
HEADER_FMT = "<IBII Q"   # 4+1+4+4+8 = 21 bytes... but struct pads. Use pack manually.
HEADER_SIZE = 4 + 1 + 4 + 4 + 8  # 21 bytes (packed)

def make_header(ptype: int, total_size: int, seq: int = 0) -> bytes:
    ts = int(time.time() * 1e9) & 0xFFFFFFFFFFFFFFFF
    return struct.pack("<IBII Q",
                       MAGIC,
                       ptype & 0xFF,
                       total_size,
                       seq,
                       ts)

def make_handshake(player_id: int, name: str, password: str) -> bytes:
    name_b   = name.encode("utf-8")[:31].ljust(32, b'\x00')
    pass_b   = password.encode("utf-8")[:63].ljust(64, b'\x00')
    version  = 1
    payload  = struct.pack("<I Q", version, player_id) + name_b + pass_b
    total    = HEADER_SIZE + len(payload)
    return make_header(PT_HANDSHAKE, total) + payload

def make_position(player_id: int, x: float, y: float, z: float,
                  rot_y: float = 0.0, seq: int = 0) -> bytes:
    payload = struct.pack("<Q ffffff I",
                          player_id, x, y, z,
                          0.0, rot_y, 0.0,   # rotX, rotY, rotZ
                          0)                  # animation
    total = HEADER_SIZE + len(payload)
    return make_header(PT_PLAYER_POS, total, seq) + payload

def make_state(player_id: int, hp: int, max_hp: int,
               souls: int = 5000, soul_level: int = 50) -> bytes:
    payload = struct.pack("<Q iiii I I",
                          player_id,
                          hp, max_hp,
                          100, 100,   # stamina, maxStamina
                          souls,
                          soul_level)
    total = HEADER_SIZE + len(payload)
    return make_header(PT_PLAYER_STATE, total) + payload

def make_heartbeat() -> bytes:
    total = HEADER_SIZE
    return make_header(PT_HEARTBEAT, total)

def make_disconnect() -> bytes:
    total = HEADER_SIZE
    return make_header(PT_DISCONNECT, total)

def make_boss_defeated(flag_id: int) -> bytes:
    payload = struct.pack("<I Q", flag_id, int(time.time()))
    total = HEADER_SIZE + len(payload)
    return make_header(PT_BOSS_DEFEATED, total) + payload

def make_souls_granted(amount: int) -> bytes:
    payload = struct.pack("<I", amount)
    total = HEADER_SIZE + len(payload)
    return make_header(PT_SOULS_GRANTED, total) + payload

# ── Packet type name (for display) ────────────────────────────────────────────
PT_NAMES = {
    0x01: "Handshake", 0x02: "Disconnect", 0x03: "Heartbeat",
    0x20: "PlayerPos", 0x22: "PlayerState", 0x30: "BossDefeated",
    0x31: "BonfireRest", 0x33: "ItemPickup", 0x34: "EventFlag",
    0x35: "SoulsGranted", 0x36: "ZoneTransition",
    0x10: "SessionCreate", 0x11: "SessionJoin", 0x12: "SessionLeave",
    0x23: "PlayerDeath", 0x24: "PlayerRespawn",
}

def pt_name(t):
    return PT_NAMES.get(t, f"0x{t:02X}")

# ── Receive thread ─────────────────────────────────────────────────────────────
_running = True

def recv_loop(sock):
    while _running:
        try:
            data, addr = sock.recvfrom(4096)
            if len(data) < HEADER_SIZE:
                continue
            magic, ptype, size, seq, ts = struct.unpack_from("<IBII Q", data, 0)
            if magic != MAGIC:
                print(f"  [RX] Paquet invalide (magic={magic:#010x}) ignoré")
                continue
            name = pt_name(ptype)
            print(f"  [RX] {name} (seq={seq}, size={size})")

            # Décode quelques types utiles
            off = HEADER_SIZE
            if ptype == PT_HANDSHAKE and len(data) >= off + 12 + 32 + 64:
                ver, pid = struct.unpack_from("<I Q", data, off)
                raw_name = data[off+12:off+12+32].split(b'\x00')[0].decode("utf-8", errors="replace")
                print(f"         → Handshake: version={ver} id={pid} name='{raw_name}'")
            elif ptype == PT_BOSS_DEFEATED and len(data) >= off + 12:
                flag_id, = struct.unpack_from("<I", data, off)
                print(f"         → Boss/Flag tué: flagId={flag_id}")
            elif ptype == PT_SOULS_GRANTED and len(data) >= off + 4:
                souls, = struct.unpack_from("<I", data, off)
                print(f"         → Âmes reçues: {souls}")
            elif ptype == PT_ZONE_TRANSIT and len(data) >= off + 5:
                bonfire_id, ttype = struct.unpack_from("<I B", data, off)
                tname = "warp" if ttype == 0 else "respawn"
                print(f"         → Zone: bonfire={bonfire_id} type={tname}")

        except socket.timeout:
            continue
        except Exception as e:
            if _running:
                print(f"  [RX] Erreur: {e}")

# ── Menu interactif ────────────────────────────────────────────────────────────
MENU = """
╔══════════════════════════════════════╗
║  DS2 Co-op — Faux Client de Test    ║
╠══════════════════════════════════════╣
║  p  → Envoyer position (cercle)     ║
║  h  → Envoyer HP aléatoire          ║
║  b  → Simuler boss tué (flag 10000) ║
║  s  → Envoyer âmes (+5000)          ║
║  z  → Simuler warp (bonfire 1020000)║
║  d  → Mort du joueur                ║
║  r  → Respawn du joueur             ║
║  q  → Déconnecter et quitter        ║
╚══════════════════════════════════════╝
"""

def main():
    parser = argparse.ArgumentParser(description="DS2 Co-op test client")
    parser.add_argument("--host",     default="127.0.0.1")
    parser.add_argument("--port",     type=int, default=27015)
    parser.add_argument("--password", default="test")
    parser.add_argument("--name",     default="TestJoueur")
    args = parser.parse_args()

    global _running

    # Génère un player ID pseudo-aléatoire
    import random
    player_id = random.randint(0x1000000, 0xFFFFFFFF)

    print(f"\n{'='*45}")
    print(f"  DS2 Seamless Co-op — Faux Client")
    print(f"  Cible  : {args.host}:{args.port}")
    print(f"  MDP    : {args.password}")
    print(f"  Nom    : {args.name}")
    print(f"  ID     : {player_id:#x}")
    print(f"{'='*45}\n")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(0.5)
    server = (args.host, args.port)

    # Envoi du handshake
    hs = make_handshake(player_id, args.name, args.password)
    sock.sendto(hs, server)
    print(f"[TX] Handshake envoyé (id={player_id:#x}, name='{args.name}')")

    # Thread de réception
    t = threading.Thread(target=recv_loop, args=(sock,), daemon=True)
    t.start()

    # Heartbeat automatique toutes les 3s
    def heartbeat_loop():
        while _running:
            time.sleep(3)
            if _running:
                sock.sendto(make_heartbeat(), server)
                print("  [TX] Heartbeat")
    threading.Thread(target=heartbeat_loop, daemon=True).start()

    # Position circulaire (pour simuler un joueur qui bouge)
    pos_angle = 0.0
    pos_seq   = 0
    # Coordonnées de base : on met une position Majula approximative
    BASE_X, BASE_Y, BASE_Z = 200.0, 0.0, 200.0

    print(MENU)

    while True:
        try:
            cmd = input("Commande > ").strip().lower()
        except (EOFError, KeyboardInterrupt):
            cmd = "q"

        if cmd == "q":
            print("[TX] Déconnexion...")
            sock.sendto(make_disconnect(), server)
            time.sleep(0.2)
            _running = False
            break

        elif cmd == "p":
            # Envoie 10 positions en cercle autour de la base
            print("[TX] Envoi de 10 positions en cercle...")
            for i in range(10):
                pos_angle += 0.3
                x = BASE_X + math.cos(pos_angle) * 3.0
                z = BASE_Z + math.sin(pos_angle) * 3.0
                pos_seq += 1
                pkt = make_position(player_id, x, BASE_Y, z, pos_angle, pos_seq)
                sock.sendto(pkt, server)
                print(f"  [TX] Position ({x:.1f}, {BASE_Y:.1f}, {z:.1f})")
                time.sleep(0.05)

        elif cmd == "h":
            import random
            hp     = random.randint(200, 1800)
            max_hp = 1800
            pkt = make_state(player_id, hp, max_hp, souls=5000, soul_level=50)
            sock.sendto(pkt, server)
            print(f"  [TX] État: HP {hp}/{max_hp}")

        elif cmd == "b":
            flag_id = 10000  # flag générique
            pkt = make_boss_defeated(flag_id)
            sock.sendto(pkt, server)
            print(f"  [TX] Boss tué (flagId={flag_id})")

        elif cmd == "s":
            amount = 5000
            pkt = make_souls_granted(amount)
            sock.sendto(pkt, server)
            print(f"  [TX] Âmes envoyées: {amount}")

        elif cmd == "z":
            # Simuler un warp vers Forest of Fallen Giants — Cardinal Tower
            bonfire_id = 1020000
            payload = struct.pack("<I B", bonfire_id, 0)
            total = HEADER_SIZE + len(payload)
            pkt = make_header(PT_ZONE_TRANSIT, total) + payload
            sock.sendto(pkt, server)
            print(f"  [TX] Zone transition vers bonfire {bonfire_id}")

        elif cmd == "d":
            # Simuler mort : envoyer PlayerDeath (0x23)
            payload = struct.pack("<Q", player_id)
            total = HEADER_SIZE + len(payload)
            pkt = make_header(0x23, total) + payload
            sock.sendto(pkt, server)
            print(f"  [TX] Mort du joueur simulée")

        elif cmd == "r":
            # Simuler respawn : envoyer PlayerRespawn (0x24)
            payload = struct.pack("<Q", player_id)
            total = HEADER_SIZE + len(payload)
            pkt = make_header(0x24, total) + payload
            sock.sendto(pkt, server)
            print(f"  [TX] Respawn du joueur simulé")

        elif cmd == "":
            pass

        else:
            print(MENU)

    sock.close()
    print("Test client terminé.")

if __name__ == "__main__":
    main()
