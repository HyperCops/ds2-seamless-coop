#pragma once

// Chiffrement P2P — AES-256-GCM via Windows BCrypt (CNG)
//
// Utilisation :
//   1. SessionCrypto::DeriveKey(password) au début de chaque session
//   2. SessionCrypto::Encrypt(plaintext, seq) avant l'envoi UDP
//   3. SessionCrypto::Decrypt(ciphertext, seq) après réception
//
// Format des paquets chiffrés :
//   [ PacketHeader (non chiffré, 17 oct.) ][ GCM tag (16 oct.) ][ payload chiffré ]
//
// Le tag GCM authentifie également le header en AAD → toute altération est détectée.

#include <cstdint>
#include <vector>
#include <string>

namespace DS2Coop::Crypto {

constexpr size_t AES_KEY_BYTES  = 32; // AES-256
constexpr size_t GCM_TAG_BYTES  = 16;
constexpr size_t GCM_NONCE_BYTES = 12;

// Dérive une clé AES-256 depuis le mot de passe de session (PBKDF2-SHA256, 10 000 itérations)
bool DeriveKey(const std::string& password);

// Chiffre `plainLen` octets de `plain` en place et écrit le tag GCM dans `outTag`.
// `seq` = numéro de séquence du paquet (nonce partiel).
// `aad` / `aadLen` = données authentifiées mais non chiffrées (le PacketHeader).
// Retourne false si la clé n'a pas été dérivée ou si BCrypt échoue.
bool Encrypt(const uint8_t* aad,    size_t aadLen,
             uint8_t*       payload, size_t payloadLen,
             uint32_t       seq,
             uint8_t        outTag[GCM_TAG_BYTES]);

// Déchiffre et vérifie le tag GCM.  Retourne false si le tag ne correspond pas.
bool Decrypt(const uint8_t* aad,    size_t aadLen,
             uint8_t*       payload, size_t payloadLen,
             uint32_t       seq,
             const uint8_t  tag[GCM_TAG_BYTES]);

// Remet à zéro la clé (appelé à la fin de la session)
void ClearKey();

bool IsKeyReady();

} // namespace DS2Coop::Crypto
