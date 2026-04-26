// Chiffrement P2P — AES-256-GCM via Windows CNG (BCrypt)
//
// Pas de dépendance externe : Windows 7+ expose BCryptDeriveKeyPBKDF2,
// BCryptEncrypt / BCryptDecrypt avec BCRYPT_CHAIN_MODE_GCM.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

#include "../../include/crypto.h"
#include "../../include/utils.h"
#include <cstring>
#include <array>

using namespace DS2Coop::Utils;

namespace DS2Coop::Crypto {

// Sel fixe — ne stocke pas de secret, sert uniquement à la dérivation de domaine.
static const char PBKDF2_SALT[] = "DS2SeamlessCoop_v1";
static constexpr uint32_t PBKDF2_ITERATIONS = 10000;

static std::array<uint8_t, AES_KEY_BYTES> g_key{};
static bool g_keyReady = false;

bool IsKeyReady() { return g_keyReady; }

void ClearKey() {
    SecureZeroMemory(g_key.data(), g_key.size());
    g_keyReady = false;
}

// ============================================================================
// Dérivation de clé via PBKDF2-SHA256 (BCryptDeriveKeyPBKDF2)
// ============================================================================
bool DeriveKey(const std::string& password) {
    ClearKey();

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (!BCRYPT_SUCCESS(status)) {
        LOG_ERROR("[CRYPTO] BCryptOpenAlgorithmProvider(SHA256/HMAC) failed: 0x%08X", status);
        return false;
    }

    status = BCryptDeriveKeyPBKDF2(
        hAlg,
        reinterpret_cast<PUCHAR>(const_cast<char*>(password.c_str())),
        static_cast<ULONG>(password.size()),
        reinterpret_cast<PUCHAR>(const_cast<char*>(PBKDF2_SALT)),
        static_cast<ULONG>(sizeof(PBKDF2_SALT) - 1),
        PBKDF2_ITERATIONS,
        g_key.data(),
        static_cast<ULONG>(g_key.size()),
        0
    );

    BCryptCloseAlgorithmProvider(hAlg, 0);

    if (!BCRYPT_SUCCESS(status)) {
        LOG_ERROR("[CRYPTO] BCryptDeriveKeyPBKDF2 failed: 0x%08X", status);
        return false;
    }

    g_keyReady = true;
    LOG_INFO("[CRYPTO] Session key derived (AES-256-GCM, PBKDF2-SHA256, %u iter)", PBKDF2_ITERATIONS);
    return true;
}

// ============================================================================
// Construit un nonce de 12 octets : seq (4 oct. LE) + timestamp (8 oct. LE)
// Le timestamp est celui embarqué dans le PacketHeader pour cohérence.
// ============================================================================
static void BuildNonce(uint32_t seq, uint64_t timestamp, uint8_t out[GCM_NONCE_BYTES]) {
    memcpy(out,     &seq,       4);
    memcpy(out + 4, &timestamp, 8);
}

// ============================================================================
// Helper : ouvre un provider AES-GCM + importe la clé dérivée
// ============================================================================
struct AesGcmCtx {
    BCRYPT_ALG_HANDLE  hAlg  = nullptr;
    BCRYPT_KEY_HANDLE  hKey  = nullptr;
    std::vector<uint8_t> keyObj;

    bool Init() {
        NTSTATUS s = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0);
        if (!BCRYPT_SUCCESS(s)) return false;

        // Mode GCM
        s = BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
            static_cast<ULONG>((wcslen(BCRYPT_CHAIN_MODE_GCM) + 1) * sizeof(wchar_t)), 0);
        if (!BCRYPT_SUCCESS(s)) { Close(); return false; }

        DWORD keyObjSize = 0, cbResult = 0;
        s = BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&keyObjSize), sizeof(keyObjSize), &cbResult, 0);
        if (!BCRYPT_SUCCESS(s)) { Close(); return false; }

        keyObj.resize(keyObjSize);
        s = BCryptGenerateSymmetricKey(hAlg, &hKey,
            keyObj.data(), keyObjSize,
            const_cast<uint8_t*>(g_key.data()), static_cast<ULONG>(g_key.size()), 0);
        if (!BCRYPT_SUCCESS(s)) { Close(); return false; }

        return true;
    }

    void Close() {
        if (hKey)  { BCryptDestroyKey(hKey);              hKey  = nullptr; }
        if (hAlg)  { BCryptCloseAlgorithmProvider(hAlg, 0); hAlg = nullptr; }
    }
};

// ============================================================================
// Chiffrement AES-256-GCM
// ============================================================================
bool Encrypt(const uint8_t* aad, size_t aadLen,
             uint8_t* payload, size_t payloadLen,
             uint32_t seq,
             uint8_t outTag[GCM_TAG_BYTES]) {
    if (!g_keyReady) return false;
    if (payloadLen == 0) return true; // rien à chiffrer

    uint64_t timestamp = 0; // sera rempli par l'appelant via le header — on passe 0 ici
    uint8_t nonce[GCM_NONCE_BYTES];
    BuildNonce(seq, timestamp, nonce);

    AesGcmCtx ctx;
    if (!ctx.Init()) {
        LOG_ERROR("[CRYPTO] Encrypt: context init failed");
        return false;
    }

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
    BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
    authInfo.pbNonce      = nonce;
    authInfo.cbNonce      = GCM_NONCE_BYTES;
    authInfo.pbAuthData   = const_cast<uint8_t*>(aad);
    authInfo.cbAuthData   = static_cast<ULONG>(aadLen);
    authInfo.pbTag        = outTag;
    authInfo.cbTag        = GCM_TAG_BYTES;

    ULONG cbResult = 0;
    NTSTATUS status = BCryptEncrypt(
        ctx.hKey,
        payload, static_cast<ULONG>(payloadLen),
        &authInfo,
        nullptr, 0,     // IV non utilisé en mode GCM (nonce dans authInfo)
        payload, static_cast<ULONG>(payloadLen),
        &cbResult, 0
    );

    ctx.Close();

    if (!BCRYPT_SUCCESS(status)) {
        LOG_ERROR("[CRYPTO] BCryptEncrypt failed: 0x%08X", status);
        return false;
    }

    return true;
}

// ============================================================================
// Déchiffrement AES-256-GCM + vérification du tag
// ============================================================================
bool Decrypt(const uint8_t* aad, size_t aadLen,
             uint8_t* payload, size_t payloadLen,
             uint32_t seq,
             const uint8_t tag[GCM_TAG_BYTES]) {
    if (!g_keyReady) return false;
    if (payloadLen == 0) return true;

    uint64_t timestamp = 0;
    uint8_t nonce[GCM_NONCE_BYTES];
    BuildNonce(seq, timestamp, nonce);

    // BCryptDecrypt modifie le tag — on travaille sur une copie locale
    uint8_t tagCopy[GCM_TAG_BYTES];
    memcpy(tagCopy, tag, GCM_TAG_BYTES);

    AesGcmCtx ctx;
    if (!ctx.Init()) {
        LOG_ERROR("[CRYPTO] Decrypt: context init failed");
        return false;
    }

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo;
    BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
    authInfo.pbNonce      = nonce;
    authInfo.cbNonce      = GCM_NONCE_BYTES;
    authInfo.pbAuthData   = const_cast<uint8_t*>(aad);
    authInfo.cbAuthData   = static_cast<ULONG>(aadLen);
    authInfo.pbTag        = tagCopy;
    authInfo.cbTag        = GCM_TAG_BYTES;

    ULONG cbResult = 0;
    NTSTATUS status = BCryptDecrypt(
        ctx.hKey,
        payload, static_cast<ULONG>(payloadLen),
        &authInfo,
        nullptr, 0,
        payload, static_cast<ULONG>(payloadLen),
        &cbResult, 0
    );

    ctx.Close();

    if (!BCRYPT_SUCCESS(status)) {
        // STATUS_AUTH_TAG_MISMATCH = 0xC000A002 — paquet corrompu ou falsifié
        LOG_WARNING("[CRYPTO] BCryptDecrypt failed (tag mismatch or error): 0x%08X", status);
        return false;
    }

    return true;
}

} // namespace DS2Coop::Crypto
