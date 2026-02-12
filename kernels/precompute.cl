#define HASH_NETNTLMV1 1
#define MAX_PLAINTEXT_LEN 8

__constant uint SB1[64] = {
    0x01010400, 0x00000000, 0x00010000, 0x01010404,
    0x01010004, 0x00010404, 0x00000004, 0x00010000,
    0x00000400, 0x01010400, 0x01010404, 0x00000400,
    0x01000404, 0x01010004, 0x01000000, 0x00000004,
    0x00000404, 0x01000400, 0x01000400, 0x00010400,
    0x00010400, 0x01010000, 0x01010000, 0x01000404,
    0x00010004, 0x01000004, 0x01000004, 0x00010004,
    0x00000000, 0x00000404, 0x00010404, 0x01000000,
    0x00010000, 0x01010404, 0x00000004, 0x01010000,
    0x01010400, 0x01000000, 0x01000000, 0x00000400,
    0x01010004, 0x00010000, 0x00010400, 0x01000004,
    0x00000400, 0x00000004, 0x01000404, 0x00010404,
    0x01010404, 0x00010004, 0x01010000, 0x01000404,
    0x01000004, 0x00000404, 0x00010404, 0x01010400,
    0x00000404, 0x01000400, 0x01000400, 0x00000000,
    0x00010004, 0x00010400, 0x00000000, 0x01010004
};

__constant uint SB2[64] = {
    0x80108020, 0x80008000, 0x00008000, 0x00108020,
    0x00100000, 0x00000020, 0x80100020, 0x80008020,
    0x80000020, 0x80108020, 0x80108000, 0x80000000,
    0x80008000, 0x00100000, 0x00000020, 0x80100020,
    0x00108000, 0x00100020, 0x80008020, 0x00000000,
    0x80000000, 0x00008000, 0x00108020, 0x80100000,
    0x00100020, 0x80000020, 0x00000000, 0x00108000,
    0x00008020, 0x80108000, 0x80100000, 0x00008020,
    0x00000000, 0x00108020, 0x80100020, 0x00100000,
    0x80008020, 0x80100000, 0x80108000, 0x00008000,
    0x80100000, 0x80008000, 0x00000020, 0x80108020,
    0x00108020, 0x00000020, 0x00008000, 0x80000000,
    0x00008020, 0x80108000, 0x00100000, 0x80000020,
    0x00100020, 0x80008020, 0x80000020, 0x00100020,
    0x00108000, 0x00000000, 0x80008000, 0x00008020,
    0x80000000, 0x80100020, 0x80108020, 0x00108000
};

__constant uint SB3[64] = {
    0x00000208, 0x08020200, 0x00000000, 0x08020008,
    0x08000200, 0x00000000, 0x00020208, 0x08000200,
    0x00020008, 0x08000008, 0x08000008, 0x00020000,
    0x08020208, 0x00020008, 0x08020000, 0x00000208,
    0x08000000, 0x00000008, 0x08020200, 0x00000200,
    0x00020200, 0x08020000, 0x08020008, 0x00020208,
    0x08000208, 0x00020200, 0x00020000, 0x08000208,
    0x00000008, 0x08020208, 0x00000200, 0x08000000,
    0x08020200, 0x08000000, 0x00020008, 0x00000208,
    0x00020000, 0x08020200, 0x08000200, 0x00000000,
    0x00000200, 0x00020008, 0x08020208, 0x08000200,
    0x08000008, 0x00000200, 0x00000000, 0x08020008,
    0x08000208, 0x00020000, 0x08000000, 0x08020208,
    0x00000008, 0x00020208, 0x00020200, 0x08000008,
    0x08020000, 0x08000208, 0x00000208, 0x08020000,
    0x00020208, 0x00000008, 0x08020008, 0x00020200
};

__constant uint SB4[64] = {
    0x00802001, 0x00002081, 0x00002081, 0x00000080,
    0x00802080, 0x00800081, 0x00800001, 0x00002001,
    0x00000000, 0x00802000, 0x00802000, 0x00802081,
    0x00000081, 0x00000000, 0x00800080, 0x00800001,
    0x00000001, 0x00002000, 0x00800000, 0x00802001,
    0x00000080, 0x00800000, 0x00002001, 0x00002080,
    0x00800081, 0x00000001, 0x00002080, 0x00800080,
    0x00002000, 0x00802080, 0x00802081, 0x00000081,
    0x00800080, 0x00800001, 0x00802000, 0x00802081,
    0x00000081, 0x00000000, 0x00000000, 0x00802000,
    0x00002080, 0x00800080, 0x00800081, 0x00000001,
    0x00802001, 0x00002081, 0x00002081, 0x00000080,
    0x00802081, 0x00000081, 0x00000001, 0x00002000,
    0x00800001, 0x00002001, 0x00802080, 0x00800081,
    0x00002001, 0x00002080, 0x00800000, 0x00802001,
    0x00000080, 0x00800000, 0x00002000, 0x00802080
};

__constant uint SB5[64] = {
    0x00000100, 0x02080100, 0x02080000, 0x42000100,
    0x00080000, 0x00000100, 0x40000000, 0x02080000,
    0x40080100, 0x00080000, 0x02000100, 0x40080100,
    0x42000100, 0x42080000, 0x00080100, 0x40000000,
    0x02000000, 0x40080000, 0x40080000, 0x00000000,
    0x40000100, 0x42080100, 0x42080100, 0x02000100,
    0x42080000, 0x40000100, 0x00000000, 0x42000000,
    0x02080100, 0x02000000, 0x42000000, 0x00080100,
    0x00080000, 0x42000100, 0x00000100, 0x02000000,
    0x40000000, 0x02080000, 0x42000100, 0x40080100,
    0x02000100, 0x40000000, 0x42080000, 0x02080100,
    0x40080100, 0x00000100, 0x02000000, 0x42080000,
    0x42080100, 0x00080100, 0x42000000, 0x42080100,
    0x02080000, 0x00000000, 0x40080000, 0x42000000,
    0x00080100, 0x02000100, 0x40000100, 0x00080000,
    0x00000000, 0x40080000, 0x02080100, 0x40000100
};

__constant uint SB6[64] = {
    0x20000010, 0x20400000, 0x00004000, 0x20404010,
    0x20400000, 0x00000010, 0x20404010, 0x00400000,
    0x20004000, 0x00404010, 0x00400000, 0x20000010,
    0x00400010, 0x20004000, 0x20000000, 0x00004010,
    0x00000000, 0x00400010, 0x20004010, 0x00004000,
    0x00404000, 0x20004010, 0x00000010, 0x20400010,
    0x20400010, 0x00000000, 0x00404010, 0x20404000,
    0x00004010, 0x00404000, 0x20404000, 0x20000000,
    0x20004000, 0x00000010, 0x20400010, 0x00404000,
    0x20404010, 0x00400000, 0x00004010, 0x20000010,
    0x00400000, 0x20004000, 0x20000000, 0x00004010,
    0x20000010, 0x20404010, 0x00404000, 0x20400000,
    0x00404010, 0x20404000, 0x00000000, 0x20400010,
    0x00000010, 0x00004000, 0x20400000, 0x00404010,
    0x00004000, 0x00400010, 0x20004010, 0x00000000,
    0x20404000, 0x20000000, 0x00400010, 0x20004010
};

__constant uint SB7[64] = {
    0x00200000, 0x04200002, 0x04000802, 0x00000000,
    0x00000800, 0x04000802, 0x00200802, 0x04200800,
    0x04200802, 0x00200000, 0x00000000, 0x04000002,
    0x00000002, 0x04000000, 0x04200002, 0x00000802,
    0x04000800, 0x00200802, 0x00200002, 0x04000800,
    0x04000002, 0x04200000, 0x04200800, 0x00200002,
    0x04200000, 0x00000800, 0x00000802, 0x04200802,
    0x00200800, 0x00000002, 0x04000000, 0x00200800,
    0x04000000, 0x00200800, 0x00200000, 0x04000802,
    0x04000802, 0x04200002, 0x04200002, 0x00000002,
    0x00200002, 0x04000000, 0x04000800, 0x00200000,
    0x04200800, 0x00000802, 0x00200802, 0x04200800,
    0x00000802, 0x04000002, 0x04200802, 0x04200000,
    0x00200800, 0x00000000, 0x00000002, 0x04200802,
    0x00000000, 0x00200802, 0x04200000, 0x00000800,
    0x04000002, 0x04000800, 0x00000800, 0x00200002
};

__constant uint SB8[64] = {
    0x10001040, 0x00001000, 0x00040000, 0x10041040,
    0x10000000, 0x10001040, 0x00000040, 0x10000000,
    0x00040040, 0x10040000, 0x10041040, 0x00041000,
    0x10041000, 0x00041040, 0x00001000, 0x00000040,
    0x10040000, 0x10000040, 0x10001000, 0x00001040,
    0x00041000, 0x00040040, 0x10040040, 0x10041000,
    0x00001040, 0x00000000, 0x00000000, 0x10040040,
    0x10000040, 0x10001000, 0x00041040, 0x00040000,
    0x00041040, 0x00040000, 0x10041000, 0x00001000,
    0x00000040, 0x10040040, 0x00001000, 0x00041040,
    0x10001000, 0x00000040, 0x10000040, 0x10040000,
    0x10040040, 0x10000000, 0x00040000, 0x10001040,
    0x00000000, 0x10041040, 0x00040040, 0x10000040,
    0x10040000, 0x10001000, 0x10001040, 0x00000000,
    0x10041040, 0x00041000, 0x00041000, 0x00001040,
    0x00001040, 0x00040040, 0x10000000, 0x10041000
};

__constant uint LHs[16] = {
    0x00000000, 0x00000001, 0x00000100, 0x00000101,
    0x00010000, 0x00010001, 0x00010100, 0x00010101,
    0x01000000, 0x01000001, 0x01000100, 0x01000101,
    0x01010000, 0x01010001, 0x01010100, 0x01010101
};

__constant uint RHs[16] = {
    0x00000000, 0x01000000, 0x00010000, 0x01010000,
    0x00000100, 0x01000100, 0x00010100, 0x01010100,
    0x00000001, 0x01000001, 0x00010001, 0x01010001,
    0x00000101, 0x01000101, 0x00010101, 0x01010101,
};

inline void des_setkey(uint *SK, uchar *key) {
    uint X, Y, T;

    X = ((uint)key[0] << 24) | ((uint)key[1] << 16) | ((uint)key[2] << 8) | key[3];
    Y = ((uint)key[4] << 24) | ((uint)key[5] << 16) | ((uint)key[6] << 8) | key[7];

    T = ((Y >> 4) ^ X) & 0x0F0F0F0F; X ^= T; Y ^= (T << 4);
    T = ((Y) ^ X) & 0x10101010; X ^= T; Y ^= (T);

    X = (LHs[(X) & 0xF] << 3) | (LHs[(X >> 8) & 0xF] << 2)
        | (LHs[(X >> 16) & 0xF] << 1) | (LHs[(X >> 24) & 0xF])
        | (LHs[(X >> 5) & 0xF] << 7) | (LHs[(X >> 13) & 0xF] << 6)
        | (LHs[(X >> 21) & 0xF] << 5) | (LHs[(X >> 29) & 0xF] << 4);

    Y = (RHs[(Y >> 1) & 0xF] << 3) | (RHs[(Y >> 9) & 0xF] << 2)
        | (RHs[(Y >> 17) & 0xF] << 1) | (RHs[(Y >> 25) & 0xF])
        | (RHs[(Y >> 4) & 0xF] << 7) | (RHs[(Y >> 12) & 0xF] << 6)
        | (RHs[(Y >> 20) & 0xF] << 5) | (RHs[(Y >> 28) & 0xF] << 4);

    X &= 0x0FFFFFFF;
    Y &= 0x0FFFFFFF;

#define DES_ROUND_KEY(i) \
    SK[i * 2] = ((X << 4) & 0x24000000) | ((X << 28) & 0x10000000) \
            | ((X << 14) & 0x08000000) | ((X << 18) & 0x02080000) \
            | ((X << 6) & 0x01000000) | ((X << 9) & 0x00200000) \
            | ((X >> 1) & 0x00100000) | ((X << 10) & 0x00040000) \
            | ((X << 2) & 0x00020000) | ((X >> 10) & 0x00010000) \
            | ((Y >> 13) & 0x00002000) | ((Y >> 4) & 0x00001000) \
            | ((Y << 6) & 0x00000800) | ((Y >> 1) & 0x00000400) \
            | ((Y >> 14) & 0x00000200) | ((Y) & 0x00000100) \
            | ((Y >> 5) & 0x00000020) | ((Y >> 10) & 0x00000010) \
            | ((Y >> 3) & 0x00000008) | ((Y >> 18) & 0x00000004) \
            | ((Y >> 26) & 0x00000002) | ((Y >> 24) & 0x00000001); \
    SK[i * 2 + 1] = ((X << 15) & 0x20000000) | ((X << 17) & 0x10000000) \
            | ((X << 10) & 0x08000000) | ((X << 22) & 0x04000000) \
            | ((X >> 2) & 0x02000000) | ((X << 1) & 0x01000000) \
            | ((X << 16) & 0x00200000) | ((X << 11) & 0x00100000) \
            | ((X << 3) & 0x00080000) | ((X >> 6) & 0x00040000) \
            | ((X << 15) & 0x00020000) | ((X >> 4) & 0x00010000) \
            | ((Y >> 2) & 0x00002000) | ((Y << 8) & 0x00001000) \
            | ((Y >> 14) & 0x00000808) | ((Y >> 9) & 0x00000400) \
            | ((Y) & 0x00000200) | ((Y << 7) & 0x00000100) \
            | ((Y >> 7) & 0x00000020) | ((Y >> 3) & 0x00000011) \
            | ((Y << 2) & 0x00000004) | ((Y >> 21) & 0x00000002);

    /* Rounds 0,1: rotate by 1 */
    X = ((X << 1) | (X >> 27)) & 0x0FFFFFFF;
    Y = ((Y << 1) | (Y >> 27)) & 0x0FFFFFFF;
    DES_ROUND_KEY(0)

    X = ((X << 1) | (X >> 27)) & 0x0FFFFFFF;
    Y = ((Y << 1) | (Y >> 27)) & 0x0FFFFFFF;
    DES_ROUND_KEY(1)

    /* Rounds 2-7: rotate by 2 */
    X = ((X << 2) | (X >> 26)) & 0x0FFFFFFF;
    Y = ((Y << 2) | (Y >> 26)) & 0x0FFFFFFF;
    DES_ROUND_KEY(2)

    X = ((X << 2) | (X >> 26)) & 0x0FFFFFFF;
    Y = ((Y << 2) | (Y >> 26)) & 0x0FFFFFFF;
    DES_ROUND_KEY(3)

    X = ((X << 2) | (X >> 26)) & 0x0FFFFFFF;
    Y = ((Y << 2) | (Y >> 26)) & 0x0FFFFFFF;
    DES_ROUND_KEY(4)

    X = ((X << 2) | (X >> 26)) & 0x0FFFFFFF;
    Y = ((Y << 2) | (Y >> 26)) & 0x0FFFFFFF;
    DES_ROUND_KEY(5)

    X = ((X << 2) | (X >> 26)) & 0x0FFFFFFF;
    Y = ((Y << 2) | (Y >> 26)) & 0x0FFFFFFF;
    DES_ROUND_KEY(6)

    X = ((X << 2) | (X >> 26)) & 0x0FFFFFFF;
    Y = ((Y << 2) | (Y >> 26)) & 0x0FFFFFFF;
    DES_ROUND_KEY(7)

    /* Round 8: rotate by 1 */
    X = ((X << 1) | (X >> 27)) & 0x0FFFFFFF;
    Y = ((Y << 1) | (Y >> 27)) & 0x0FFFFFFF;
    DES_ROUND_KEY(8)

    /* Rounds 9-14: rotate by 2 */
    X = ((X << 2) | (X >> 26)) & 0x0FFFFFFF;
    Y = ((Y << 2) | (Y >> 26)) & 0x0FFFFFFF;
    DES_ROUND_KEY(9)

    X = ((X << 2) | (X >> 26)) & 0x0FFFFFFF;
    Y = ((Y << 2) | (Y >> 26)) & 0x0FFFFFFF;
    DES_ROUND_KEY(10)

    X = ((X << 2) | (X >> 26)) & 0x0FFFFFFF;
    Y = ((Y << 2) | (Y >> 26)) & 0x0FFFFFFF;
    DES_ROUND_KEY(11)

    X = ((X << 2) | (X >> 26)) & 0x0FFFFFFF;
    Y = ((Y << 2) | (Y >> 26)) & 0x0FFFFFFF;
    DES_ROUND_KEY(12)

    X = ((X << 2) | (X >> 26)) & 0x0FFFFFFF;
    Y = ((Y << 2) | (Y >> 26)) & 0x0FFFFFFF;
    DES_ROUND_KEY(13)

    X = ((X << 2) | (X >> 26)) & 0x0FFFFFFF;
    Y = ((Y << 2) | (Y >> 26)) & 0x0FFFFFFF;
    DES_ROUND_KEY(14)

    /* Round 15: rotate by 1 */
    X = ((X << 1) | (X >> 27)) & 0x0FFFFFFF;
    Y = ((Y << 1) | (Y >> 27)) & 0x0FFFFFFF;
    DES_ROUND_KEY(15)

#undef DES_ROUND_KEY
}

inline void des_encrypt(uint *SK, uchar *output) {
    uint X, Y, T;

    X = 0xf0aaf0aa;
    Y = 0x00cd00cd;

#define DES_HALF_ROUND(a, b, k) \
    T = SK[k] ^ (b); \
    (a) ^= SB8[(T) & 0x3F] ^ SB6[(T >> 8) & 0x3F] ^ \
           SB4[(T >> 16) & 0x3F] ^ SB2[(T >> 24) & 0x3F]; \
    T = SK[k + 1] ^ rotate((b), 28u); \
    (a) ^= SB7[(T) & 0x3F] ^ SB5[(T >> 8) & 0x3F] ^ \
           SB3[(T >> 16) & 0x3F] ^ SB1[(T >> 24) & 0x3F];

    DES_HALF_ROUND(X, Y, 0)
    DES_HALF_ROUND(Y, X, 2)
    DES_HALF_ROUND(X, Y, 4)
    DES_HALF_ROUND(Y, X, 6)
    DES_HALF_ROUND(X, Y, 8)
    DES_HALF_ROUND(Y, X, 10)
    DES_HALF_ROUND(X, Y, 12)
    DES_HALF_ROUND(Y, X, 14)
    DES_HALF_ROUND(X, Y, 16)
    DES_HALF_ROUND(Y, X, 18)
    DES_HALF_ROUND(X, Y, 20)
    DES_HALF_ROUND(Y, X, 22)
    DES_HALF_ROUND(X, Y, 24)
    DES_HALF_ROUND(Y, X, 26)
    DES_HALF_ROUND(X, Y, 28)
    DES_HALF_ROUND(Y, X, 30)

#undef DES_HALF_ROUND

    Y = rotate(Y, 31u);
    T = (Y ^ X) & 0xAAAAAAAA; Y ^= T; X ^= T;
    X = rotate(X, 31u);
    T = ((X >> 8) ^ Y) & 0x00FF00FF; Y ^= T; X ^= (T << 8);
    T = ((X >> 2) ^ Y) & 0x33333333; Y ^= T; X ^= (T << 2);
    T = ((Y >> 16) ^ X) & 0x0000FFFF; X ^= T; Y ^= (T << 16);
    T = ((Y >> 4) ^ X) & 0x0F0F0F0F; X ^= T; Y ^= (T << 4);

    output[0] = Y >> 24; output[1] = Y >> 16; output[2] = Y >> 8; output[3] = Y;
    output[4] = X >> 24; output[5] = X >> 16; output[6] = X >> 8; output[7] = X;
}

inline void netntlmv1_hash(uchar *key_56, uchar *output) {
    uchar key[8];
    uint SK[32];

    key[0] = (((key_56[0] >> 1) & 0x7f) << 1);
    key[1] = (((key_56[0] & 0x01) << 6 | ((key_56[1] >> 2) & 0x3f)) << 1);
    key[2] = (((key_56[1] & 0x03) << 5 | ((key_56[2] >> 3) & 0x1f)) << 1);
    key[3] = (((key_56[2] & 0x07) << 4 | ((key_56[3] >> 4) & 0x0f)) << 1);
    key[4] = (((key_56[3] & 0x0f) << 3 | ((key_56[4] >> 5) & 0x07)) << 1);
    key[5] = (((key_56[4] & 0x1f) << 2 | ((key_56[5] >> 6) & 0x03)) << 1);
    key[6] = (((key_56[5] & 0x3f) << 1 | ((key_56[6] >> 7) & 0x01)) << 1);
    key[7] = ((key_56[6] & 0x7f) << 1);

    des_setkey(SK, key);
    des_encrypt(SK, output);
}

/* Fused: index -> DES hash -> next index. Avoids byte array intermediaries. */
inline ulong index_hash_reduce(ulong index, uint reduction_offset, ulong plaintext_space_mask, uint pos) {
    /* index_to_plaintext: extract 7 bytes from 56-bit index (big-endian) */
    uchar k0 = (uchar)(index >> 48);
    uchar k1 = (uchar)(index >> 40);
    uchar k2 = (uchar)(index >> 32);
    uchar k3 = (uchar)(index >> 24);
    uchar k4 = (uchar)(index >> 16);
    uchar k5 = (uchar)(index >> 8);
    uchar k6 = (uchar)(index);

    /* netntlmv1 key expansion: 56-bit -> 64-bit with parity */
    uchar key[8];
    key[0] = (((k0 >> 1) & 0x7f) << 1);
    key[1] = (((k0 & 0x01) << 6 | ((k1 >> 2) & 0x3f)) << 1);
    key[2] = (((k1 & 0x03) << 5 | ((k2 >> 3) & 0x1f)) << 1);
    key[3] = (((k2 & 0x07) << 4 | ((k3 >> 4) & 0x0f)) << 1);
    key[4] = (((k3 & 0x0f) << 3 | ((k4 >> 5) & 0x07)) << 1);
    key[5] = (((k4 & 0x1f) << 2 | ((k5 >> 6) & 0x03)) << 1);
    key[6] = (((k5 & 0x3f) << 1 | ((k6 >> 7) & 0x01)) << 1);
    key[7] = ((k6 & 0x7f) << 1);

    uint SK[32];
    des_setkey(SK, key);

    /* des_encrypt inlined to return hash as ulong directly */
    uint X, Y, T;
    X = 0xf0aaf0aa;
    Y = 0x00cd00cd;

#define DES_HR(a, b, ki) \
    T = SK[ki] ^ (b); \
    (a) ^= SB8[(T) & 0x3F] ^ SB6[(T >> 8) & 0x3F] ^ \
           SB4[(T >> 16) & 0x3F] ^ SB2[(T >> 24) & 0x3F]; \
    T = SK[ki + 1] ^ rotate((b), 28u); \
    (a) ^= SB7[(T) & 0x3F] ^ SB5[(T >> 8) & 0x3F] ^ \
           SB3[(T >> 16) & 0x3F] ^ SB1[(T >> 24) & 0x3F];

    DES_HR(X, Y, 0)  DES_HR(Y, X, 2)  DES_HR(X, Y, 4)  DES_HR(Y, X, 6)
    DES_HR(X, Y, 8)  DES_HR(Y, X, 10) DES_HR(X, Y, 12) DES_HR(Y, X, 14)
    DES_HR(X, Y, 16) DES_HR(Y, X, 18) DES_HR(X, Y, 20) DES_HR(Y, X, 22)
    DES_HR(X, Y, 24) DES_HR(Y, X, 26) DES_HR(X, Y, 28) DES_HR(Y, X, 30)

#undef DES_HR

    Y = rotate(Y, 31u);
    T = (Y ^ X) & 0xAAAAAAAA; Y ^= T; X ^= T;
    X = rotate(X, 31u);
    T = ((X >> 8) ^ Y) & 0x00FF00FF; Y ^= T; X ^= (T << 8);
    T = ((X >> 2) ^ Y) & 0x33333333; Y ^= T; X ^= (T << 2);
    T = ((Y >> 16) ^ X) & 0x0000FFFF; X ^= T; Y ^= (T << 16);
    T = ((Y >> 4) ^ X) & 0x0F0F0F0F; X ^= T; Y ^= (T << 4);

    /* hash_to_index: hash bytes are [Y>>24,Y>>16,Y>>8,Y,X>>24,X>>16,X>>8,X]
     * We need ret = hash[7]<<56 | hash[6]<<48 | ... | hash[0]
     * = X<<56 | (X>>8&0xFF)<<48 | (X>>16&0xFF)<<40 | (X>>24)<<32
     *   | Y<<24 | (Y>>8&0xFF)<<16 | (Y>>16&0xFF)<<8 | (Y>>24) */
    ulong ret = ((ulong)(X & 0xFF) << 56) | ((ulong)((X >> 8) & 0xFF) << 48) |
                ((ulong)((X >> 16) & 0xFF) << 40) | ((ulong)(X >> 24) << 32) |
                ((ulong)(Y & 0xFF) << 24) | ((ulong)((Y >> 8) & 0xFF) << 16) |
                ((ulong)((Y >> 16) & 0xFF) << 8) | (ulong)(Y >> 24);

    return (ret + reduction_offset + pos) & plaintext_space_mask;
}

inline ulong hash_to_index(uchar *hash, uint reduction_offset, ulong plaintext_space_mask, uint pos) {
    ulong ret = (ulong)hash[7] << 56 | (ulong)hash[6] << 48 |
                (ulong)hash[5] << 40 | (ulong)hash[4] << 32 |
                (ulong)hash[3] << 24 | (ulong)hash[2] << 16 |
                (ulong)hash[1] << 8  | (ulong)hash[0];
    return (ret + reduction_offset + pos) & plaintext_space_mask;
}

inline void index_to_plaintext(ulong index, uchar *key_out) {
    key_out[6] = (uchar)index; index >>= 8;
    key_out[5] = (uchar)index; index >>= 8;
    key_out[4] = (uchar)index; index >>= 8;
    key_out[3] = (uchar)index; index >>= 8;
    key_out[2] = (uchar)index; index >>= 8;
    key_out[1] = (uchar)index; index >>= 8;
    key_out[0] = (uchar)index;
}

__kernel void precompute(
    __global uchar *g_hash,
    uint chain_len,
    uint reduction_offset,
    ulong plaintext_space_mask,
    __global ulong *g_output
) {
    uint pos = get_global_id(0);

    if (pos >= chain_len - 1) {
        return;
    }

    uchar hash[8];
    uchar key[7];
    ulong index;

    for (int i = 0; i < 8; i++) {
        hash[i] = g_hash[i];
    }

    index = hash_to_index(hash, reduction_offset, plaintext_space_mask, pos);

    for (uint p = pos + 1; p < chain_len - 1; p++) {
        index = index_hash_reduce(index, reduction_offset, plaintext_space_mask, p);
    }

    g_output[pos] = index;
}

__kernel void precompute_init(
    __global uchar *g_hash,
    uint chain_len,
    uint reduction_offset,
    ulong plaintext_space_mask,
    __global ulong *g_indices,
    uint pos_offset
) {
    uint gid = get_global_id(0);
    uint pos = gid + pos_offset;
    if (pos >= chain_len - 1) return;

    uchar hash[8];
    for (int i = 0; i < 8; i++) hash[i] = g_hash[i];

    g_indices[gid] = hash_to_index(hash, reduction_offset, plaintext_space_mask, pos);
}

__kernel void precompute_step(
    __global ulong *g_indices,
    uint num_positions,
    uint round_num,
    uint stride,
    uint chain_len,
    uint reduction_offset,
    ulong plaintext_space_mask,
    uint pos_offset
) {
    uint gid = get_global_id(0);
    if (gid >= num_positions) return;

    uint pos = gid + pos_offset;
    uint total_ops = chain_len - 2 - pos;
    uint ops_done = round_num * stride;
    if (ops_done >= total_ops) return;

    uint ops_this_round = stride;
    if (ops_done + ops_this_round > total_ops)
        ops_this_round = total_ops - ops_done;

    ulong index = g_indices[gid];
    uint p_start = pos + 1 + ops_done;
    uint p_end = p_start + ops_this_round;

    for (uint p = p_start; p < p_end; p++) {
        index = index_hash_reduce(index, reduction_offset, plaintext_space_mask, p);
    }

    g_indices[gid] = index;
}