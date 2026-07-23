#include "AesGcmSoft.h"
#include <string.h>

static const uint8_t SBOX[256] = {
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

static const uint8_t RCON[10] = {0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36};

static inline uint8_t xtime(uint8_t x) { return (uint8_t)((x<<1)^((x>>7)?0x1b:0)); }

bool AesGcmSoft::setKey(const uint8_t key[CRYPTO_KEY_SIZE]) {
    memcpy(_rk, key, 16);
    for (int i = 1; i <= 10; i++) {
        const uint8_t* p = _rk + (i-1)*16;
        uint8_t*       c = _rk + i*16;
        uint8_t t0=SBOX[p[13]]^RCON[i-1], t1=SBOX[p[14]], t2=SBOX[p[15]], t3=SBOX[p[12]];
        c[0]=p[0]^t0;   c[1]=p[1]^t1;   c[2]=p[2]^t2;   c[3]=p[3]^t3;
        c[4]=p[4]^c[0]; c[5]=p[5]^c[1]; c[6]=p[6]^c[2]; c[7]=p[7]^c[3];
        c[8]=p[8]^c[4]; c[9]=p[9]^c[5]; c[10]=p[10]^c[6]; c[11]=p[11]^c[7];
        c[12]=p[12]^c[8]; c[13]=p[13]^c[9]; c[14]=p[14]^c[10]; c[15]=p[15]^c[11];
    }
    _ready = true;
    return true;
}

void AesGcmSoft::_aesBlock(const uint8_t in[16], uint8_t out[16]) const {
    uint8_t s[16];
    memcpy(s, in, 16);
    for (int i = 0; i < 16; i++) s[i] ^= _rk[i];

    for (int r = 1; r <= 9; r++) {
        for (int i = 0; i < 16; i++) s[i] = SBOX[s[i]];
        uint8_t t;
        t=s[1];  s[1]=s[5];  s[5]=s[9];  s[9]=s[13];  s[13]=t;
        t=s[2];  s[2]=s[10]; s[10]=t;   t=s[6]; s[6]=s[14]; s[14]=t;
        t=s[15]; s[15]=s[11]; s[11]=s[7]; s[7]=s[3]; s[3]=t;
        for (int c = 0; c < 4; c++) {
            uint8_t a=s[c*4],b=s[c*4+1],cc=s[c*4+2],d=s[c*4+3];
            s[c*4]  =xtime(a)^xtime(b)^b^cc^d;
            s[c*4+1]=a^xtime(b)^xtime(cc)^cc^d;
            s[c*4+2]=a^b^xtime(cc)^xtime(d)^d;
            s[c*4+3]=xtime(a)^a^b^cc^xtime(d);
        }
        for (int i = 0; i < 16; i++) s[i] ^= _rk[r*16+i];
    }
    for (int i = 0; i < 16; i++) s[i] = SBOX[s[i]];
    {
        uint8_t t;
        t=s[1];  s[1]=s[5];  s[5]=s[9];  s[9]=s[13];  s[13]=t;
        t=s[2];  s[2]=s[10]; s[10]=t;   t=s[6]; s[6]=s[14]; s[14]=t;
        t=s[15]; s[15]=s[11]; s[11]=s[7]; s[7]=s[3]; s[3]=t;
    }
    for (int i = 0; i < 16; i++) s[i] ^= _rk[160+i];
    memcpy(out, s, 16);
}

// GF(2^128) multiply: Z = X · Y  (NIST SP 800-38D, MSB-first)
static void ghashMul(const uint8_t X[16], const uint8_t Y[16], uint8_t Z[16]) {
    uint8_t z[16]={}, v[16];
    memcpy(v, X, 16);
    for (int i = 0; i < 16; i++) {
        uint8_t y = Y[i];
        for (int bit = 7; bit >= 0; bit--) {
            if ((y>>bit)&1) for (int k=0;k<16;k++) z[k]^=v[k];
            uint8_t lsb = v[15]&1;
            for (int k=15;k>0;k--) v[k]=(uint8_t)((v[k]>>1)|(v[k-1]<<7));
            v[0]>>=1;
            if (lsb) v[0]^=0xe1;
        }
    }
    memcpy(Z, z, 16);
}

static void ghashUpdate(uint8_t Y[16], const uint8_t H[16],
                         const uint8_t* data, size_t len) {
    const uint8_t* p = data;
    while (len >= 16) {
        for (int i=0;i<16;i++) Y[i]^=p[i];
        uint8_t t[16]; memcpy(t,Y,16); ghashMul(t,H,Y);
        p+=16; len-=16;
    }
    if (len > 0) {
        uint8_t tmp[16]={};
        memcpy(tmp,p,len);
        for (int i=0;i<16;i++) Y[i]^=tmp[i];
        uint8_t t[16]; memcpy(t,Y,16); ghashMul(t,H,Y);
    }
}

void AesGcmSoft::_ctrCrypt(const uint8_t J0[16], uint8_t* data, size_t len) const {
    uint8_t ctr[16];
    memcpy(ctr, J0, 16);
    for (size_t i = 0; i < len; ) {
        uint32_t cnt = ((uint32_t)ctr[12]<<24)|((uint32_t)ctr[13]<<16)|
                       ((uint32_t)ctr[14]<< 8)| ctr[15];
        cnt++;
        ctr[12]=(uint8_t)(cnt>>24); ctr[13]=(uint8_t)(cnt>>16);
        ctr[14]=(uint8_t)(cnt>> 8); ctr[15]=(uint8_t)cnt;
        uint8_t ks[16]; _aesBlock(ctr,ks);
        size_t n=(len-i<16)?(len-i):16;
        for (size_t j=0;j<n;j++) data[i+j]^=ks[j];
        i+=n;
    }
}

void AesGcmSoft::_computeTag(const uint8_t H[16], const uint8_t EJ0[16],
                               const uint8_t* aad, size_t aadLen,
                               const uint8_t* C, size_t cLen,
                               uint8_t tag[CRYPTO_TAG_SIZE]) const {
    uint8_t Y[16]={};
    ghashUpdate(Y,H,aad,aadLen);
    ghashUpdate(Y,H,C,cLen);
    uint8_t lenBlock[16]={};
    uint64_t aadBits=(uint64_t)aadLen*8, cBits=(uint64_t)cLen*8;
    for (int i=0;i<8;i++) {
        lenBlock[i]  =(uint8_t)((aadBits>>(56-i*8))&0xFF);
        lenBlock[8+i]=(uint8_t)((cBits  >>(56-i*8))&0xFF);
    }
    ghashUpdate(Y,H,lenBlock,16);
    for (size_t i=0;i<CRYPTO_TAG_SIZE;i++) tag[i]=EJ0[i]^Y[i];
}

bool AesGcmSoft::encrypt(const uint8_t* aad, size_t aadLen,
                          uint8_t* data, size_t dataLen,
                          const uint8_t nonce[CRYPTO_NONCE_SIZE],
                          uint8_t tag[CRYPTO_TAG_SIZE]) {
    if (!_ready) return false;
    uint8_t H[16]={};  _aesBlock(H,H);
    uint8_t J0[16]={}; memcpy(J0,nonce,CRYPTO_NONCE_SIZE); J0[15]=1;
    uint8_t EJ0[16];   _aesBlock(J0,EJ0);
    _ctrCrypt(J0,data,dataLen);
    _computeTag(H,EJ0,aad,aadLen,data,dataLen,tag);
    return true;
}

bool AesGcmSoft::decrypt(const uint8_t* aad, size_t aadLen,
                          uint8_t* data, size_t dataLen,
                          const uint8_t nonce[CRYPTO_NONCE_SIZE],
                          const uint8_t tag[CRYPTO_TAG_SIZE]) {
    if (!_ready) return false;
    uint8_t H[16]={};  _aesBlock(H,H);
    uint8_t J0[16]={}; memcpy(J0,nonce,CRYPTO_NONCE_SIZE); J0[15]=1;
    uint8_t EJ0[16];   _aesBlock(J0,EJ0);
    uint8_t expectedTag[CRYPTO_TAG_SIZE];
    _computeTag(H,EJ0,aad,aadLen,data,dataLen,expectedTag);
    // Constant-time compare.
    uint8_t diff=0;
    for (size_t i=0;i<CRYPTO_TAG_SIZE;i++) diff|=expectedTag[i]^tag[i];
    if (diff!=0) { memset(data,0,dataLen); return false; }
    _ctrCrypt(J0,data,dataLen);
    return true;
}
