/*
 * receiver.c - Phone-band robust BFSK receiver
 * Usage:
 *   ./receiver encoded_signal.wav
 *
 * Steps:
 * 1) Load mono float
 * 2) DC remove + normalize
 * 3) Bandpass (rough) around 700..2600 Hz to reduce speech/music junk
 * 4) Find preamble by scanning offsets (1010...) + try invert
 * 5) Refine around boundary by searching MAGIC "STEG"
 * 6) Decode frame using REP majority vote
 * 7) CRC check then AES-CTR decrypt
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <openssl/evp.h>
#include <sndfile.h>

/* Must match sender */
#define FREQ_0          1200.0
#define FREQ_1          2200.0
#define BIT_DURATION    0.015
#define PREAMBLE_SECONDS 1.5
#define REP             3

/* Search params */
#define SEARCH_SECONDS   3.0
#define SEARCH_STEP_FRAC 6     /* step = spb/6 */
#define REFINE_STEPS     24    /* refine +-spb with spb/REFINE_STEPS */

static unsigned char key[32] = "01234567890123456789012345678901";
static unsigned char iv[16]  = "0123456789012345";

/* ---------- AES-CTR decrypt ---------- */
static int decrypt_aes_ctr(const unsigned char *cipher, int clen,
                           unsigned char *plain, int cap)
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if(!ctx) return -1;

    int len=0, outlen=0;
    if(EVP_DecryptInit_ex(ctx, EVP_aes_256_ctr(), NULL, key, iv) != 1) goto fail;
    if(EVP_DecryptUpdate(ctx, plain, &len, cipher, clen) != 1) goto fail;
    outlen = len;
    if(EVP_DecryptFinal_ex(ctx, plain + outlen, &len) != 1) goto fail;
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

/* ---------- WAV load mono ---------- */
static float *load_mono(const char *path, int *out_n, int *out_fs){
    *out_n=0; *out_fs=0;

    SF_INFO info; memset(&info,0,sizeof(info));
    SNDFILE *f = sf_open(path, SFM_READ, &info);
    if(!f) return NULL;
    if(info.frames<=0 || info.channels<=0){ sf_close(f); return NULL; }

    *out_fs = info.samplerate;

    sf_count_t frames = info.frames;
    int ch = info.channels;
    sf_count_t total = frames * ch;

    float *tmp = (float*)malloc((size_t)total*sizeof(float));
    if(!tmp){ sf_close(f); return NULL; }

    sf_count_t r = sf_read_float(f, tmp, total);
    sf_close(f);
    if(r <= 0){ free(tmp); return NULL; }

    float *mono = (float*)malloc((size_t)frames*sizeof(float));
    if(!mono){ free(tmp); return NULL; }

    for(int i=0;i<(int)frames;i++){
        double sum=0.0;
        for(int c=0;c<ch;c++){
            sf_count_t idx=(sf_count_t)i*ch + c;
            if(idx < r) sum += tmp[idx];
        }
        mono[i]=(float)(sum/ch);
    }
    free(tmp);

    *out_n = (int)frames;
    return mono;
}

static float rmsf(const float *x, int n){
    double s=0.0;
    for(int i=0;i<n;i++){
        double v=x[i];
        s += v*v;
    }
    return (float)sqrt(s/(double)(n>0?n:1));
}

/* DC remove + normalize to fixed RMS */
static void preprocess(float *x, int n){
    if(n<=0) return;

    /* remove DC */
    double mean=0.0;
    for(int i=0;i<n;i++) mean += x[i];
    mean /= (double)n;
    for(int i=0;i<n;i++) x[i] = (float)(x[i] - mean);

    /* normalize RMS */
    float r = rmsf(x, n);
    if(r < 1e-6f) return;
    float target = 0.25f;
    float g = target / r;
    for(int i=0;i<n;i++) x[i] *= g;
}

/* Simple biquad (RBJ) */
typedef struct {
    double b0,b1,b2,a1,a2;
    double z1,z2;
} biquad;

static biquad rbj_lowpass(int fs, double f0, double Q){
    double w0 = 2.0*M_PI*f0/(double)fs;
    double alpha = sin(w0)/(2.0*Q);
    double c = cos(w0);

    double b0=(1-c)/2, b1=1-c, b2=(1-c)/2;
    double a0=1+alpha, a1=-2*c, a2=1-alpha;

    biquad q;
    q.b0=b0/a0; q.b1=b1/a0; q.b2=b2/a0;
    q.a1=a1/a0; q.a2=a2/a0;
    q.z1=0; q.z2=0;
    return q;
}

static biquad rbj_highpass(int fs, double f0, double Q){
    double w0 = 2.0*M_PI*f0/(double)fs;
    double alpha = sin(w0)/(2.0*Q);
    double c = cos(w0);

    double b0=(1+c)/2, b1=-(1+c), b2=(1+c)/2;
    double a0=1+alpha, a1=-2*c, a2=1-alpha;

    biquad q;
    q.b0=b0/a0; q.b1=b1/a0; q.b2=b2/a0;
    q.a1=a1/a0; q.a2=a2/a0;
    q.z1=0; q.z2=0;
    return q;
}

static void biquad_process(biquad *q, float *x, int n){
    double z1=q->z1, z2=q->z2;
    for(int i=0;i<n;i++){
        double in=x[i];
        double out = q->b0*in + z1;
        z1 = q->b1*in - q->a1*out + z2;
        z2 = q->b2*in - q->a2*out;
        x[i] = (float)out;
    }
    q->z1=z1; q->z2=z2;
}

/* Bandpass around 700..2600 Hz (helps phone-band BFSK) */
static void bandpass(float *x, int n, int fs){
    biquad hp = rbj_highpass(fs, 700.0, 0.707);
    biquad lp = rbj_lowpass(fs, 2600.0, 0.707);
    biquad_process(&hp, x, n);
    biquad_process(&lp, x, n);
}

/* I/Q energy compare, phase-robust */
static int detect_bit_q(const float *x, long long start, int len, int fs, int invert){
    double i0=0,q0=0,i1=0,q1=0;
    double w0=2.0*M_PI*FREQ_0/(double)fs;
    double w1=2.0*M_PI*FREQ_1/(double)fs;

    for(int n=0;n<len;n++){
        double s = x[start + n];

        i0 += s * cos(w0*n);  q0 += s * sin(w0*n);
        i1 += s * cos(w1*n);  q1 += s * sin(w1*n);
    }

    double p0=i0*i0+q0*q0;
    double p1=i1*i1+q1*q1;

    int bit = (p1 > p0) ? 1 : 0;
    if(invert) bit ^= 1;
    return bit;
}

static int decode_coded_bit(const float *x, long long pos, int spb, int fs, int invert){
    int ones=0;
    for(int r=0;r<REP;r++){
        int b = detect_bit_q(x, pos + (long long)r*spb, spb, fs, invert);
        ones += b;
    }
    return (ones > (REP/2)) ? 1 : 0;
}

static uint8_t decode_byte(const float *x, long long *pos, int spb, int fs, int invert){
    uint8_t v=0;
    for(int k=0;k<8;k++){
        int b = decode_coded_bit(x, *pos, spb, fs, invert);
        v = (uint8_t)((v<<1) | (uint8_t)(b & 1));
        *pos += (long long)REP * (long long)spb;
    }
    return v;
}

/* score preamble match at offset */
static int score_preamble(const float *x, int n, long long off, int spb, int fs, int pre_bits, int invert){
    int score=0;
    for(int b=0;b<pre_bits;b++){
        long long pos = off + (long long)b*spb;
        if(pos + spb >= n) break;
        int expected = (b & 1) ? 1 : 0; /* 1010... */
        int got = detect_bit_q(x, pos, spb, fs, invert);
        if(got == expected) score++;
    }
    return score;
}

int main(int argc, char **argv){
    if(argc < 2){
        fprintf(stderr, "Usage: %s <file.wav>\n", argv[0]);
        return 1;
    }

    crc32_init();

    int n=0, fs=0;
    float *x = load_mono(argv[1], &n, &fs);
    if(!x){
        fprintf(stderr, "Failed to load wav\n");
        return 1;
    }

    preprocess(x, n);
    bandpass(x, n, fs);

    int spb = (int)lround((double)fs * (double)BIT_DURATION);
    if(spb < 40){
        fprintf(stderr, "BIT_DURATION too small or fs weird\n");
        free(x);
        return 1;
    }

    int pre_bits = (int)lround(PREAMBLE_SECONDS / BIT_DURATION);
    if(pre_bits < 32) pre_bits = 32;

    long long search_max = (long long)lround(SEARCH_SECONDS * (double)fs);
    if(search_max > n) search_max = n;

    long long step = spb / SEARCH_STEP_FRAC;
    if(step < 1) step = 1;

    long long best_off=-1;
    int best_inv=0;
    int best_score=-1;

    for(long long off=0; off + (long long)pre_bits*spb < search_max; off += step){
        int s0 = score_preamble(x, n, off, spb, fs, pre_bits, 0);
        if(s0 > best_score){ best_score=s0; best_off=off; best_inv=0; }

        int s1 = score_preamble(x, n, off, spb, fs, pre_bits, 1);
        if(s1 > best_score){ best_score=s1; best_off=off; best_inv=1; }

        if(best_score > (int)(0.93 * pre_bits)) break;
    }

    if(best_off < 0){
        fprintf(stderr, "Sync not found\n");
        free(x);
        return 1;
    }

    /* refine around boundary by scanning for MAGIC "STEG" */
    long long base = best_off + (long long)pre_bits * (long long)spb;

    long long best_pos = -1;
    int best_inv2 = best_inv;

    long long step2 = spb / REFINE_STEPS;
    if(step2 < 1) step2 = 1;

    for(long long delta = -spb; delta <= spb; delta += step2){
        for(int inv_try=0; inv_try<=1; inv_try++){
            long long p = base + delta;
            if(p < 0) continue;
            if(p + (long long)4 * (long long)REP * (long long)spb * 8LL >= n) continue;

            long long tmp = p;
            unsigned char m0 = decode_byte(x, &tmp, spb, fs, inv_try);
            unsigned char m1 = decode_byte(x, &tmp, spb, fs, inv_try);
            unsigned char m2 = decode_byte(x, &tmp, spb, fs, inv_try);
            unsigned char m3 = decode_byte(x, &tmp, spb, fs, inv_try);

            if(m0=='S' && m1=='T' && m2=='E' && m3=='G'){
                best_pos = p;
                best_inv2 = inv_try;
                goto FOUND;
            }
        }
    }

FOUND:
    if(best_pos < 0){
        fprintf(stderr, "MAGIC not found near sync. score=%d/%d\n", best_score, pre_bits);
        free(x);
        return 1;
    }

    long long pos = best_pos;
    int invert = best_inv2;

    /* decode header: MAGIC+LEN */
    unsigned char hdr[8];
    for(int i=0;i<8;i++) hdr[i] = decode_byte(x, &pos, spb, fs, invert);

    if(!(hdr[0]=='S' && hdr[1]=='T' && hdr[2]=='E' && hdr[3]=='G')){
        fprintf(stderr, "MAGIC mismatch (should not happen after refine)\n");
        fprintf(stderr, "Got: %02X %02X %02X %02X\n", hdr[0],hdr[1],hdr[2],hdr[3]);
        free(x);
        return 1;
    }

    uint32_t clen = ((uint32_t)hdr[4]<<24) | ((uint32_t)hdr[5]<<16) | ((uint32_t)hdr[6]<<8) | (uint32_t)hdr[7];
    if(clen == 0 || clen > 2000000u){
        fprintf(stderr, "Invalid LEN: %u\n", clen);
        free(x);
        return 1;
    }

    size_t frame_no_crc = 8 + (size_t)clen;
    unsigned char *frame = (unsigned char*)malloc(frame_no_crc + 4);
    if(!frame){ free(x); return 1; }
    memcpy(frame, hdr, 8);

    for(uint32_t i=0;i<clen;i++){
        frame[8+i] = decode_byte(x, &pos, spb, fs, invert);
    }

    unsigned char crc_bytes[4];
    for(int i=0;i<4;i++) crc_bytes[i] = decode_byte(x, &pos, spb, fs, invert);

    uint32_t crc_stored = ((uint32_t)crc_bytes[0]<<24) | ((uint32_t)crc_bytes[1]<<16) | ((uint32_t)crc_bytes[2]<<8) | (uint32_t)crc_bytes[3];
    uint32_t crc_calc = crc32_compute(frame, frame_no_crc);

    if(crc_calc != crc_stored){
        fprintf(stderr, "CRC mismatch (data corrupted)\n");
        fprintf(stderr, "calc=%08X stored=%08X\n", crc_calc, crc_stored);
        fprintf(stderr, "Sync: off=%lld inv=%d score=%d/%d\n", best_off, best_inv, best_score, pre_bits);
        free(frame);
        free(x);
        return 1;
    }

    /* decrypt ciphertext (frame+8 .. frame+8+clen-1) */
    unsigned char *plain = (unsigned char*)malloc((size_t)clen + 64);
    if(!plain){ free(frame); free(x); return 1; }

    int plen = decrypt_aes_ctr(frame+8, (int)clen, plain, (int)clen + 64);
    if(plen < 0){
        fprintf(stderr, "Decrypt failed\n");
        free(plain);
        free(frame);
        free(x);
        return 1;
    }
    plain[plen] = 0;


    printf("Sync: off=%lld samples (inv=%d score=%d/%d)\n", best_off, best_inv, best_score, pre_bits);
    printf("Refined pos=%lld samples (inv=%d)\n", best_pos, invert);
    printf("Decrypted Message:\n%s\n", plain);

    free(plain);
    free(frame);
    free(x);
    return 0;
}
