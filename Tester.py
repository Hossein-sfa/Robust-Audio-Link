import argparse
import numpy as np
import soundfile as sf
import math

def clamp(x):
    return np.clip(x, -1.0, 1.0).astype(np.float32)

def to_mono(audio: np.ndarray) -> np.ndarray:
    if audio.ndim == 1:
        return audio.astype(np.float32)
    return np.mean(audio, axis=1).astype(np.float32)

def rms(x: np.ndarray) -> float:
    return float(np.sqrt(np.mean(x * x)) + 1e-12)

# -------- Resample (fast + dependency-free) --------
def resample_linear(x: np.ndarray, fs_in: int, fs_out: int) -> np.ndarray:
    if fs_in == fs_out:
        return x.astype(np.float32)
    n_in = len(x)
    dur = n_in / float(fs_in)
    n_out = int(round(dur * fs_out))
    if n_out <= 1:
        return x[:1].astype(np.float32)
    t_in = np.linspace(0.0, dur, n_in, endpoint=False, dtype=np.float64)
    t_out = np.linspace(0.0, dur, n_out, endpoint=False, dtype=np.float64)
    y = np.interp(t_out, t_in, x.astype(np.float64)).astype(np.float32)
    return y

# -------- Simple RBJ biquad filters --------
class Biquad:
    def __init__(self, b0, b1, b2, a1, a2):
        self.b0, self.b1, self.b2 = float(b0), float(b1), float(b2)
        self.a1, self.a2 = float(a1), float(a2)
        self.z1, self.z2 = 0.0, 0.0

    def process(self, x: np.ndarray) -> np.ndarray:
        y = np.zeros_like(x, dtype=np.float32)
        z1, z2 = self.z1, self.z2
        b0, b1, b2, a1, a2 = self.b0, self.b1, self.b2, self.a1, self.a2
        for i in range(len(x)):
            xi = float(x[i])
            yi = b0 * xi + z1
            z1 = b1 * xi - a1 * yi + z2
            z2 = b2 * xi - a2 * yi
            y[i] = yi
        self.z1, self.z2 = z1, z2
        return y

def rbj_lowpass(fs, f0, Q=0.707):
    w0 = 2.0 * math.pi * f0 / fs
    alpha = math.sin(w0) / (2.0 * Q)
    c = math.cos(w0)

    b0 = (1 - c) / 2
    b1 = 1 - c
    b2 = (1 - c) / 2
    a0 = 1 + alpha
    a1 = -2 * c
    a2 = 1 - alpha
    return Biquad(b0/a0, b1/a0, b2/a0, a1/a0, a2/a0)

def rbj_highpass(fs, f0, Q=0.707):
    w0 = 2.0 * math.pi * f0 / fs
    alpha = math.sin(w0) / (2.0 * Q)
    c = math.cos(w0)

    b0 = (1 + c) / 2
    b1 = -(1 + c)
    b2 = (1 + c) / 2
    a0 = 1 + alpha
    a1 = -2 * c
    a2 = 1 - alpha
    return Biquad(b0/a0, b1/a0, b2/a0, a1/a0, a2/a0)

def bandlimit_telephony(x: np.ndarray, fs: int, lo=300.0, hi=3400.0) -> np.ndarray:
    hp = rbj_highpass(fs, lo, Q=0.707)
    lp = rbj_lowpass(fs, hi, Q=0.707)
    y = hp.process(x)
    y = lp.process(y)
    return y

# -------- μ-law (G.711) --------
def linear_to_ulaw_sample(x):
    pcm = int(np.round(np.clip(x, -1.0, 1.0) * 32767.0))
    cBias = 0x84
    cClip = 32635
    sign = 1 if pcm < 0 else 0
    mag = -pcm if sign else pcm
    mag = min(mag, cClip)
    mag += cBias

    exp = 7
    mask = 0x4000
    while exp > 0 and (mag & mask) == 0:
        exp -= 1
        mask >>= 1

    mant = (mag >> (exp + 3)) & 0x0F
    ulaw = (~((sign << 7) | (exp << 4) | mant)) & 0xFF
    return ulaw

def ulaw_to_linear_sample(u):
    u = (~u) & 0xFF
    sign = (u & 0x80) != 0
    exp = (u >> 4) & 0x07
    mant = u & 0x0F
    mag = ((mant << 3) + 0x84) << exp
    pcm = -(mag - 0x84) if sign else (mag - 0x84)
    return float(pcm) / 32767.0

def apply_mulaw(x: np.ndarray) -> np.ndarray:
    out = np.zeros_like(x, dtype=np.float32)
    for i in range(len(x)):
        u = linear_to_ulaw_sample(float(x[i]))
        out[i] = ulaw_to_linear_sample(u)
    return out

# -------- Quantization (bit-depth simulation) --------
def quantize(x: np.ndarray, bits: int) -> np.ndarray:
    if bits >= 16:
        return x.astype(np.float32)
    levels = float((1 << bits) - 1)
    y = np.round((clamp(x) * 0.5 + 0.5) * levels) / levels
    y = (y - 0.5) * 2.0
    return clamp(y)

# -------- Noise models --------
def add_awgn(x: np.ndarray, snr_db: float, rng: np.random.Generator) -> np.ndarray:
    if snr_db > 200:
        return x.astype(np.float32)
    sig_r = rms(x)
    snr_lin = 10.0 ** (snr_db / 20.0)
    noise_r = sig_r / snr_lin
    n = rng.normal(0.0, noise_r, size=x.shape).astype(np.float32)
    return clamp(x + n)

def add_pink_noise(x: np.ndarray, snr_db: float, rng: np.random.Generator) -> np.ndarray:
    if snr_db > 200:
        return x.astype(np.float32)
    w = rng.normal(0.0, 1.0, size=x.shape).astype(np.float32)
    y = np.zeros_like(w, dtype=np.float32)
    a = 0.98
    acc = 0.0
    for i in range(len(w)):
        acc = a * acc + (1.0 - a) * float(w[i])
        y[i] = acc
    y = y / (rms(y) + 1e-12)
    sig_r = rms(x)
    snr_lin = 10.0 ** (snr_db / 20.0)
    noise_r = sig_r / snr_lin
    return clamp(x + y * noise_r)

def add_hum(x: np.ndarray, snr_db: float, fs: int, freq_hz: float, harmonics: int, rng: np.random.Generator) -> np.ndarray:
    if snr_db > 200:
        return x.astype(np.float32)
    t = (np.arange(len(x), dtype=np.float32) / float(fs))
    phase = float(rng.random() * 2.0 * np.pi)
    hum = np.zeros_like(x, dtype=np.float32)
    for k in range(1, harmonics + 1):
        hum += (1.0 / k) * np.sin(2.0 * np.pi * (freq_hz * k) * t + phase).astype(np.float32)
    hum = hum / (rms(hum) + 1e-12)
    sig_r = rms(x)
    snr_lin = 10.0 ** (snr_db / 20.0)
    noise_r = sig_r / snr_lin
    return clamp(x + hum * noise_r)

def add_clicks(x: np.ndarray, snr_db: float, fs: int, rate_hz: float, click_ms: float, rng: np.random.Generator) -> np.ndarray:
    if snr_db > 200:
        return x.astype(np.float32)
    n = len(x)
    clicks = np.zeros(n, dtype=np.float32)
    expected = int(rate_hz * (n / fs))
    L = max(1, int(fs * (click_ms / 1000.0)))
    for _ in range(expected):
        pos = int(rng.integers(0, max(1, n - L)))
        amp = float(rng.uniform(-1.0, 1.0))
        pulse = amp * np.exp(-np.linspace(0, 6, L, dtype=np.float32))
        clicks[pos:pos+L] += pulse.astype(np.float32)
    if rms(clicks) < 1e-9:
        return x.astype(np.float32)
    clicks = clicks / (rms(clicks) + 1e-12)
    sig_r = rms(x)
    snr_lin = 10.0 ** (snr_db / 20.0)
    noise_r = sig_r / snr_lin
    return clamp(x + clicks * noise_r)

def apply_noise(x, fs, noise_type, snr_db, rng):
    if noise_type == "awgn":
        return add_awgn(x, snr_db, rng)
    if noise_type == "pink":
        return add_pink_noise(x, snr_db, rng)
    if noise_type == "hum":
        return add_hum(x, snr_db, fs, freq_hz=50.0, harmonics=5, rng=rng)
    if noise_type == "clicks":
        return add_clicks(x, snr_db, fs, rate_hz=2.0, click_ms=3.0, rng=rng)
    if noise_type == "mix":
        y = add_awgn(x, snr_db + 3.0, rng)
        y = add_hum(y, snr_db + 6.0, fs, freq_hz=50.0, harmonics=3, rng=rng)
        y = add_clicks(y, snr_db + 6.0, fs, rate_hz=1.0, click_ms=2.0, rng=rng)
        return clamp(y)
    raise SystemExit("Unknown noise type. Use: awgn|pink|hum|clicks|mix")

# -------- Compression-ish presets --------
def apply_compression_preset(x: np.ndarray, fs: int, preset: str) -> np.ndarray:
    preset = preset.lower()

    if preset == "none":
        return x.astype(np.float32)

    if preset == "voip":
        # mild codec-ish: bandlimit to ~7k, downsample to 16k, quantize 12-bit, back to fs
        y = bandlimit_telephony(x, fs, lo=80.0, hi=7000.0)
        y16 = resample_linear(y, fs, 16000)
        y16 = quantize(y16, 12)
        y = resample_linear(y16, 16000, fs)
        return clamp(y)

    if preset == "pstn":
        # phone line: 300-3400, 8kHz sample rate, μ-law 8-bit
        y = bandlimit_telephony(x, fs, lo=300.0, hi=3400.0)
        y8 = resample_linear(y, fs, 8000)
        y8 = apply_mulaw(y8)
        y = resample_linear(y8, 8000, fs)
        return clamp(y)

    if preset == "lowbit":
        # harsh: bandlimit 6k, downsample 12k, quantize 8-bit
        y = bandlimit_telephony(x, fs, lo=120.0, hi=6000.0)
        y12 = resample_linear(y, fs, 12000)
        y12 = quantize(y12, 8)
        y = resample_linear(y12, 12000, fs)
        return clamp(y)

    raise SystemExit("Unknown preset. Use: none|voip|pstn|lowbit")

def main():
    p = argparse.ArgumentParser(
        description="Channel stress test: codec-like compression + noise (defaults set so you don't have to think)."
    )
    p.add_argument("in_wav", help="input wav")
    p.add_argument("out_wav", help="output wav")
    p.add_argument("--preset", default="voip", choices=["none", "voip", "pstn", "lowbit"],
                   help="compression preset (default: voip)")
    p.add_argument("--noise", default="mix", choices=["awgn", "pink", "hum", "clicks", "mix"],
                   help="noise type (default: mix)")
    p.add_argument("--snr", type=float, default=18.0, help="SNR in dB (default: 18)")
    p.add_argument("--seed", type=int, default=123, help="random seed (default: 123)")
    args = p.parse_args()

    audio, fs = sf.read(args.in_wav, dtype="float32")
    x = to_mono(audio)

    rng = np.random.default_rng(args.seed)

    # 1) compression-like damage
    y = apply_compression_preset(x, fs, args.preset)

    # 2) noise damage
    y = apply_noise(y, fs, args.noise, args.snr, rng)

    sf.write(args.out_wav, y, fs, subtype="PCM_16")
    print(f"OK: wrote {args.out_wav} (preset={args.preset}, noise={args.noise}, snr={args.snr} dB, fs={fs}, seed={args.seed})")

if __name__ == "__main__":
    main()
