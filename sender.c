/*
 * sender.c - Phone-band robust BFSK sender
 * Usage:
 *   ./sender "message"               -> outputs encoded_signal.wav (pure BFSK)
 *   ./sender "message" cover.wav     -> outputs encoded_signal.wav (BFSK mixed into cover)
 *
 * Design:
 * - BFSK in phone band: FREQ_0=1200, FREQ_1=2200
 * - BIT_DURATION=30ms, REP=3
 * - Preamble: 1.5s of 1010...
 * - Frame: "STEG" + LEN(4, BE) + CIPHERTEXT + CRC32(frame_without_crc)
 *
 * NOTE: For best results on real phone:
 * - Keep output WAV mono 44100
 * - When playing over speaker: disable "noise suppression / voice enhancement" if possible
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <openssl/evp.h>
#include <sndfile.h>

/* ---------- TX params ---------- */
#define SAMPLE_RATE     44100
#define FREQ_0          1200.0
#define FREQ_1          2200.0
#define BIT_DURATION    0.015
#define PREAMBLE_SECONDS 1.5

#define REP             3             // repetition coding (majority decode)
#define AMPLITUDE       0.87f         // base BFSK amplitude (pure mode)
#define STEGO_STRENGTH  0.2f         // BFSK scale when mixing with cover
#define COVER_GAIN      0.3f         // cover scale when mixing

/* AES key/IV (fixed for demo). Real-world: random IV per message + transmit IV. */
static unsigned char key[32] = "01234567890123456789012345678901";
static unsigned char iv[16]  = "0123456789012345";

/* ---------- AES-CTR encrypt ---------- */
static int encrypt_aes_ctr(const unsigned char *plain, int plen,
                           unsigned char *cipher, int cap)
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if(!ctx) return -1;

    int len=0, outlen=0;

    if(EVP_EncryptInit_ex(ctx, EVP_aes_256_ctr(), NULL, key, iv) != 1) goto fail;
    if(EVP_EncryptUpdate(ctx, cipher, &len, plain, plen) != 1) goto fail;
    outlen = len;
    if(EVP_EncryptFinal_ex(ctx, cipher + outlen, &len) != 1) goto fail;
    outlen += len;

    EVP_CIPHER_CTX_free(ctx);
    return (outlen <= cap) ? outlen : -1;

fail:
    EVP_CIPHER_CTX_free(ctx);
    return -1;
}

/* ---------- CRC32 ---------- */
static uint32_t crc32_table[256];

static void crc32_init(void){
    uint32_t poly = 0xEDB88320u;
    for(uint32_t i=0;i<256;i++){
        uint32_t c=i;
        for(int k=0;k<8;k++) c = (c & 1u) ? (poly ^ (c >> 1)) : (c >> 1);
        crc32_table[i]=c;
    }
}

static uint32_t crc32_compute(const uint8_t *data, size_t n){
    uint32_t c=0xFFFFFFFFu;
    for(size_t i=0;i<n;i++) c = crc32_table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ---------- WAV cover loader (mono) ---------- */
static float *load_cover_mono(const char *path, int *out_len){
    *out_len = 0;

    SF_INFO info; memset(&info,0,sizeof(info));
    SNDFILE *f = sf_open(path, SFM_READ, &info);
    if(!f) return NULL;

    if(info.frames<=0 || info.channels<=0){
        sf_close(f);
        return NULL;
    }

    sf_count_t frames = info.frames;
    int ch = info.channels;
    sf_count_t total = frames * ch;

    float *tmp = (float*)malloc((size_t)total * sizeof(float));
    if(!tmp){ sf_close(f); return NULL; }

    sf_count_t r = sf_read_float(f, tmp, total);
    sf_close(f);
    if(r <= 0){ free(tmp); return NULL; }

    float *mono = (float*)malloc((size_t)frames * sizeof(float));
    if(!mono){ free(tmp); return NULL; }

    for(int i=0;i<(int)frames;i++){
        double sum=0.0;
        for(int c=0;c<ch;c++){
            sf_count_t idx=(sf_count_t)i*ch+c;
            if(idx < r) sum += tmp[idx];
        }
        mono[i]=(float)(sum/ch);
    }

    free(tmp);
    *out_len = (int)frames;
    return mono;
}

static float clampf(float x){
    if(x>1.f) return 1.f;
    if(x<-1.f) return -1.f;
    return x;
}

/* Hann window to reduce spectral splatter (helps codecs a bit) */
static float hann(int n, int N){
    if(N <= 1) return 1.f;
    return 0.5f - 0.5f*cosf(2.0f*(float)M_PI*(float)n/(float)(N-1));
}

int main(int argc, char **argv){
    if(argc < 2){
        fprintf(stderr, "Usage: %s \"message\" [cover.wav]\n", argv[0]);
        return 1;
    }

    const char *msg = argv[1];
    const char *cover_path = (argc >= 3) ? argv[2] : NULL;

    crc32_init();

    /* encrypt */
    int msg_len = (int)strlen(msg);
    unsigned char *cipher = (unsigned char*)malloc((size_t)msg_len + 64);
    if(!cipher){ perror("malloc cipher"); return 1; }

    int clen = encrypt_aes_ctr((const unsigned char*)msg, msg_len, cipher, msg_len + 64);
    if(clen < 0){
        fprintf(stderr, "Encrypt failed\n");
        free(cipher);
        return 1;
    }

    /* frame: STEG + LEN + CIPHER + CRC32 */
    size_t frame_no_crc = 4 + 4 + (size_t)clen;
    size_t frame_total  = frame_no_crc + 4;

    unsigned char *frame = (unsigned char*)malloc(frame_total);
    if(!frame){ perror("malloc frame"); free(cipher); return 1; }

    frame[0]='S'; frame[1]='T'; frame[2]='E'; frame[3]='G';
    frame[4]=(clen>>24)&0xFF; frame[5]=(clen>>16)&0xFF; frame[6]=(clen>>8)&0xFF; frame[7]=(clen)&0xFF;
    memcpy(frame+8, cipher, (size_t)clen);

    uint32_t crc = crc32_compute(frame, frame_no_crc);
    frame[frame_no_crc+0] = (crc>>24)&0xFF;
    frame[frame_no_crc+1] = (crc>>16)&0xFF;
    frame[frame_no_crc+2] = (crc>>8)&0xFF;
    frame[frame_no_crc+3] = (crc)&0xFF;

    free(cipher);

    int spb = (int)lround((double)SAMPLE_RATE * (double)BIT_DURATION);
    if(spb < 40){
        fprintf(stderr, "BIT_DURATION too small\n");
        free(frame);
        return 1;
    }

    int pre_bits = (int)lround(PREAMBLE_SECONDS / BIT_DURATION);
    if(pre_bits < 32) pre_bits = 32;

    long long data_bits = (long long)frame_total * 8LL;
    long long tx_bits   = (long long)pre_bits + data_bits * (long long)REP;
    long long total_samples = tx_bits * (long long)spb;

    /* cover */
    float *cover = NULL;
    int cover_len = 0;
    int use_cover = 0;

    if(cover_path){
        cover = load_cover_mono(cover_path, &cover_len);
        if(!cover || cover_len <= 0){
            fprintf(stderr, "Warning: cover load failed -> pure BFSK\n");
            if(cover) free(cover);
            cover = NULL;
            cover_len = 0;
        } else {
            use_cover = 1;
            fprintf(stderr, "Cover loaded: %s (mono samples=%d)\n", cover_path, cover_len);
        }
    }

    /* output wav */
    SF_INFO out; memset(&out,0,sizeof(out));
    out.samplerate = SAMPLE_RATE;
    out.channels = 1;
    out.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

    const char *out_path = "encoded_signal.wav";
    SNDFILE *fo = sf_open(out_path, SFM_WRITE, &out);
    if(!fo){
        fprintf(stderr, "Failed to open output wav\n");
        free(frame);
        if(cover) free(cover);
        return 1;
    }

    float *buf = (float*)malloc((size_t)total_samples * sizeof(float));
    if(!buf){
        perror("malloc buf");
        sf_close(fo);
        free(frame);
        if(cover) free(cover);
        return 1;
    }

    long long si = 0;

    /* 1) preamble 1010... */
    for(int b=0;b<pre_bits;b++){
        int bit = (b & 1) ? 1 : 0;
        float freq = bit ? (float)FREQ_1 : (float)FREQ_0;

        for(int s=0;s<spb;s++){
            float t = (float)si / (float)SAMPLE_RATE;
            float w = hann(s, spb);
            float tone = sinf(2.0f*(float)M_PI*freq*t);
            float sig  = AMPLITUDE * w * tone;

            float y;
            if(use_cover){
                float base = cover[(int)(si % cover_len)];
                y = COVER_GAIN * base + STEGO_STRENGTH * sig;
            } else {
                y = sig;
            }
            buf[si++] = clampf(y);
        }
    }

    /* 2) data bits with repetition */
    for(size_t i=0;i<frame_total;i++){
        for(int bitpos=7; bitpos>=0; bitpos--){
            int bit = (frame[i] >> bitpos) & 1;
            float freq = bit ? (float)FREQ_1 : (float)FREQ_0;

            for(int r=0;r<REP;r++){
                for(int s=0;s<spb;s++){
                    float t = (float)si / (float)SAMPLE_RATE;
                    float w = hann(s, spb);
                    float tone = sinf(2.0f*(float)M_PI*freq*t);
                    float sig  = AMPLITUDE * w * tone;

                    float y;
                    if(use_cover){
                        float base = cover[(int)(si % cover_len)];
                        y = COVER_GAIN * base + STEGO_STRENGTH * sig;
                    } else {
                        y = sig;
                    }
                    buf[si++] = clampf(y);
                }
            }
        }
    }

    sf_write_float(fo, buf, (sf_count_t)total_samples);
    sf_close(fo);

    printf("OK: wrote %s\n", out_path);
    printf("Duration: %.1f sec\n", (double)total_samples / (double)SAMPLE_RATE);

    free(buf);
    free(frame);
    if(cover) free(cover);
    return 0;
}
