/* SHA-1 (FIPS 180-1), used to check that the loaded program is the
 * SLUS_006.62 the decomp matches. */
#include <stdint.h>
#include <string.h>

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void block(uint32_t h[5], const unsigned char *p) {
    uint32_t w[80], a, b, c, d, e, t;
    int i;
    for (i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    }
    for (i = 16; i < 80; i++) {
        w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4];
    for (i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        t = ROL(a, 5) + f + e + k + w[i];
        e = d; d = c; c = ROL(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void pe_host_sha1(const void *data, unsigned int len, unsigned char out[20]) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    const unsigned char *p = (const unsigned char *)data;
    unsigned char tail[128];
    unsigned int i, rem = len % 64, tl;
    uint64_t bits = (uint64_t)len * 8;
    for (i = 0; i + 64 <= len; i += 64) {
        block(h, p + i);
    }
    memset(tail, 0, sizeof(tail));
    memcpy(tail, p + i, rem);
    tail[rem] = 0x80;
    tl = rem < 56 ? 64 : 128;
    for (i = 0; i < 8; i++) {
        tail[tl - 1 - i] = (unsigned char)(bits >> (8 * i));
    }
    block(h, tail);
    if (tl == 128) {
        block(h, tail + 64);
    }
    for (i = 0; i < 5; i++) {
        out[i * 4] = (unsigned char)(h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)h[i];
    }
}
