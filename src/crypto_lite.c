/*
 * crypto_lite.c - Minimal AES-128-ECB and SHA-256 for PSP Moonlight pairing
 *
 * Standard implementations of FIPS 197 (AES) and FIPS 180-4 (SHA-256).
 * No external dependencies beyond string.h.
 */

#include "crypto_lite.h"
#include <string.h>

/* =========================================================================
 * SHA-256 Implementation (FIPS 180-4)
 * ========================================================================= */

static const unsigned int sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

#define SHA_ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define SHA_CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define SHA_MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define SHA_EP0(x) (SHA_ROR(x, 2) ^ SHA_ROR(x, 13) ^ SHA_ROR(x, 22))
#define SHA_EP1(x) (SHA_ROR(x, 6) ^ SHA_ROR(x, 11) ^ SHA_ROR(x, 25))
#define SHA_SIG0(x) (SHA_ROR(x, 7) ^ SHA_ROR(x, 18) ^ ((x) >> 3))
#define SHA_SIG1(x) (SHA_ROR(x, 17) ^ SHA_ROR(x, 19) ^ ((x) >> 10))

static void sha256_transform(unsigned int state[8], const unsigned char block[64])
{
    unsigned int a, b, c, d, e, f, g, h, t1, t2, w[64];
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((unsigned int)block[i * 4] << 24) |
               ((unsigned int)block[i * 4 + 1] << 16) |
               ((unsigned int)block[i * 4 + 2] << 8) |
               ((unsigned int)block[i * 4 + 3]);
    }
    for (i = 16; i < 64; i++) {
        w[i] = SHA_SIG1(w[i - 2]) + w[i - 7] + SHA_SIG0(w[i - 15]) + w[i - 16];
    }

    a = state[0]; b = state[1]; c = state[2]; d = state[3];
    e = state[4]; f = state[5]; g = state[6]; h = state[7];

    for (i = 0; i < 64; i++) {
        t1 = h + SHA_EP1(e) + SHA_CH(e, f, g) + sha256_k[i] + w[i];
        t2 = SHA_EP0(a) + SHA_MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void sha256_hash(const unsigned char *data, size_t len, unsigned char *out)
{
    unsigned int state[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    unsigned char block[64];
    size_t i, rem;
    unsigned long long bitlen = (unsigned long long)len * 8;

    /* Process full 64-byte blocks */
    for (i = 0; i + 64 <= len; i += 64) {
        sha256_transform(state, data + i);
    }

    /* Remaining bytes + padding */
    rem = len - i;
    memset(block, 0, 64);
    if (rem > 0) {
        memcpy(block, data + i, rem);
    }
    block[rem] = 0x80;

    if (rem >= 56) {
        sha256_transform(state, block);
        memset(block, 0, 64);
    }

    /* Append length in bits (big-endian) */
    block[56] = (unsigned char)(bitlen >> 56);
    block[57] = (unsigned char)(bitlen >> 48);
    block[58] = (unsigned char)(bitlen >> 40);
    block[59] = (unsigned char)(bitlen >> 32);
    block[60] = (unsigned char)(bitlen >> 24);
    block[61] = (unsigned char)(bitlen >> 16);
    block[62] = (unsigned char)(bitlen >> 8);
    block[63] = (unsigned char)(bitlen);
    sha256_transform(state, block);

    /* Output hash (big-endian) */
    for (i = 0; i < 8; i++) {
        out[i * 4]     = (unsigned char)(state[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(state[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(state[i] >> 8);
        out[i * 4 + 3] = (unsigned char)(state[i]);
    }
}

/* =========================================================================
 * AES-128-ECB Implementation (FIPS 197)
 * ========================================================================= */

static const unsigned char aes_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static const unsigned char aes_inv_sbox[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};

static const unsigned char aes_rcon[11] = {
    0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36
};

#define AES_Nb 4
#define AES_Nk 4
#define AES_Nr 10

static void aes_key_expansion(const unsigned char *key, unsigned char rk[176])
{
    unsigned char tmp[4];
    int i;

    memcpy(rk, key, 16);

    for (i = AES_Nk; i < AES_Nb * (AES_Nr + 1); i++) {
        memcpy(tmp, rk + (i - 1) * 4, 4);
        if (i % AES_Nk == 0) {
            unsigned char t = tmp[0];
            tmp[0] = aes_sbox[tmp[1]] ^ aes_rcon[i / AES_Nk];
            tmp[1] = aes_sbox[tmp[2]];
            tmp[2] = aes_sbox[tmp[3]];
            tmp[3] = aes_sbox[t];
        }
        rk[i * 4]     = rk[(i - AES_Nk) * 4]     ^ tmp[0];
        rk[i * 4 + 1] = rk[(i - AES_Nk) * 4 + 1] ^ tmp[1];
        rk[i * 4 + 2] = rk[(i - AES_Nk) * 4 + 2] ^ tmp[2];
        rk[i * 4 + 3] = rk[(i - AES_Nk) * 4 + 3] ^ tmp[3];
    }
}

static unsigned char xtime(unsigned char x)
{
    return (unsigned char)((x << 1) ^ (((x >> 7) & 1) * 0x1b));
}

static void aes_encrypt_block(const unsigned char in[16], unsigned char out[16],
                               const unsigned char rk[176])
{
    unsigned char s[16];
    int i, r;

    memcpy(s, in, 16);

    /* AddRoundKey - initial */
    for (i = 0; i < 16; i++) s[i] ^= rk[i];

    for (r = 1; r <= AES_Nr; r++) {
        unsigned char t[16];

        /* SubBytes */
        for (i = 0; i < 16; i++) s[i] = aes_sbox[s[i]];

        /* ShiftRows */
        t[0] =s[0]; t[1] =s[5]; t[2] =s[10]; t[3] =s[15];
        t[4] =s[4]; t[5] =s[9]; t[6] =s[14]; t[7] =s[3];
        t[8] =s[8]; t[9] =s[13]; t[10]=s[2];  t[11]=s[7];
        t[12]=s[12]; t[13]=s[1]; t[14]=s[6];  t[15]=s[11];
        memcpy(s, t, 16);

        /* MixColumns (skip in last round) */
        if (r < AES_Nr) {
            for (i = 0; i < 16; i += 4) {
                unsigned char a0 = s[i], a1 = s[i+1], a2 = s[i+2], a3 = s[i+3];
                unsigned char h = a0 ^ a1 ^ a2 ^ a3;
                s[i]   = a0 ^ xtime(a0 ^ a1) ^ h;
                s[i+1] = a1 ^ xtime(a1 ^ a2) ^ h;
                s[i+2] = a2 ^ xtime(a2 ^ a3) ^ h;
                s[i+3] = a3 ^ xtime(a3 ^ a0) ^ h;
            }
        }

        /* AddRoundKey */
        for (i = 0; i < 16; i++) s[i] ^= rk[r * 16 + i];
    }

    memcpy(out, s, 16);
}

static void aes_decrypt_block(const unsigned char in[16], unsigned char out[16],
                               const unsigned char rk[176])
{
    unsigned char s[16];
    int i, r;

    memcpy(s, in, 16);

    /* AddRoundKey - last round key */
    for (i = 0; i < 16; i++) s[i] ^= rk[AES_Nr * 16 + i];

    for (r = AES_Nr - 1; r >= 0; r--) {
        unsigned char t[16];

        /* InvShiftRows */
        t[0] =s[0]; t[1] =s[13]; t[2] =s[10]; t[3] =s[7];
        t[4] =s[4]; t[5] =s[1];  t[6] =s[14]; t[7] =s[11];
        t[8] =s[8]; t[9] =s[5];  t[10]=s[2];  t[11]=s[15];
        t[12]=s[12]; t[13]=s[9]; t[14]=s[6];  t[15]=s[3];
        memcpy(s, t, 16);

        /* InvSubBytes */
        for (i = 0; i < 16; i++) s[i] = aes_inv_sbox[s[i]];

        /* AddRoundKey */
        for (i = 0; i < 16; i++) s[i] ^= rk[r * 16 + i];

        /* InvMixColumns (skip in round 0) */
        if (r > 0) {
            for (i = 0; i < 16; i += 4) {
                unsigned char a0 = s[i], a1 = s[i+1], a2 = s[i+2], a3 = s[i+3];
                unsigned char u = xtime(xtime(a0 ^ a2));
                unsigned char v = xtime(xtime(a1 ^ a3));
                s[i]   ^= u; s[i+1] ^= v; s[i+2] ^= u; s[i+3] ^= v;
                /* Now apply forward MixColumns to complete inverse */
                {
                    unsigned char h = s[i] ^ s[i+1] ^ s[i+2] ^ s[i+3];
                    unsigned char b0 = s[i];
                    s[i]   = s[i]   ^ xtime(s[i] ^ s[i+1])   ^ h;
                    s[i+1] = s[i+1] ^ xtime(s[i+1] ^ s[i+2]) ^ h;
                    s[i+2] = s[i+2] ^ xtime(s[i+2] ^ s[i+3]) ^ h;
                    s[i+3] = s[i+3] ^ xtime(s[i+3] ^ b0)     ^ h;
                }
            }
        }
    }

    memcpy(out, s, 16);
}

void aes128_ecb_encrypt(const unsigned char *plaintext, int len,
                        const unsigned char *key, unsigned char *ciphertext)
{
    unsigned char rk[176];
    int i;
    aes_key_expansion(key, rk);
    for (i = 0; i < len; i += 16) {
        aes_encrypt_block(plaintext + i, ciphertext + i, rk);
    }
}

void aes128_ecb_decrypt(const unsigned char *ciphertext, int len,
                        const unsigned char *key, unsigned char *plaintext)
{
    unsigned char rk[176];
    int i;
    aes_key_expansion(key, rk);
    for (i = 0; i < len; i += 16) {
        aes_decrypt_block(ciphertext + i, plaintext + i, rk);
    }
}

/* =========================================================================
 * Hex Conversion
 * ========================================================================= */

static const char hex_lut[16] = "0123456789abcdef";

void bytes_to_hex_lite(const unsigned char *in, char *out, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        out[i * 2]     = hex_lut[(in[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hex_lut[in[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

void hex_to_bytes_lite(const char *in, unsigned char *out, size_t hex_len)
{
    size_t i;
    for (i = 0; i < hex_len; i += 2) {
        unsigned char hi = (in[i] >= '0' && in[i] <= '9') ? (in[i] - '0') :
                           (in[i] >= 'a' && in[i] <= 'f') ? (in[i] - 'a' + 10) :
                           (in[i] - 'A' + 10);
        unsigned char lo = (in[i+1] >= '0' && in[i+1] <= '9') ? (in[i+1] - '0') :
                           (in[i+1] >= 'a' && in[i+1] <= 'f') ? (in[i+1] - 'a' + 10) :
                           (in[i+1] - 'A' + 10);
        out[i / 2] = (hi << 4) | lo;
    }
}
