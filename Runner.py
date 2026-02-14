import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import List, Tuple

# Core helpers

def run_cmd(cmd: List[str], cwd: Path, timeout: int = 300) -> Tuple[int, str, str]:
    p = subprocess.run(
        cmd,
        cwd=str(cwd),
        capture_output=True,
        text=True,
        timeout=timeout
    )
    return p.returncode, p.stdout, p.stderr

def ensure_dir(p: Path) -> None:
    p.mkdir(parents=True, exist_ok=True)

def read_text_file(p: Path) -> str:
    return p.read_text(encoding="utf-8", errors="ignore").strip()

def normalize_text(s: str) -> str:
    s = s.replace("\r\n", "\n").replace("\r", "\n").strip()
    s = re.sub(r"[ \t]+", " ", s)
    s = re.sub(r"\n+", "\n", s)
    return s

def word_count(s: str) -> int:
    return len([w for w in re.split(r"\s+", s.strip()) if w])

def parse_receiver_decrypted(stdout: str) -> str:
    m = re.search(r"Decrypted Message:\s*\n(.*)\Z", stdout, re.DOTALL)
    if m:
        return m.group(1).strip()
    return stdout.strip()

def similarity_quick(a: str, b: str) -> float:
    a = normalize_text(a)
    b = normalize_text(b)
    if not a and not b:
        return 1.0
    if not a or not b:
        return 0.0
    n = min(len(a), len(b))
    eq = sum(1 for i in range(n) if a[i] == b[i])
    return eq / max(len(a), len(b))

def slug(s: str) -> str:
    s = re.sub(r"[^a-zA-Z0-9._-]+", "_", s.strip())
    return re.sub(r"_+", "_", s).strip("_")

# Test matrix (noise + "compression-ish" presets from Tester.py)

@dataclass
class Case:
    preset: str   # none|voip|pstn|lowbit
    noise: str    # awgn|pink|hum|clicks|mix
    snr: float
    seed: int

DEFAULT_CASES: List[Case] = [
    Case("none",   "awgn",   40.0, 123),
    Case("none",   "awgn",   25.0, 123),

    Case("voip",   "mix",    22.0, 123),
    Case("voip",   "mix",    18.0, 123),
    Case("voip",   "pink",   18.0, 123),

    Case("pstn",   "mix",    18.0, 123),
    Case("pstn",   "hum",    20.0, 123),

    Case("lowbit", "mix",    22.0, 123),
    Case("lowbit", "clicks", 25.0, 123),
]


def main():
    ap = argparse.ArgumentParser(description="Encrypt->channel stress(noise+compression)->decrypt, save WAVs, compare with message.txt.")
    ap.add_argument("--sender", default="sender", help="Sender executable filename/path (default: sender)")
    ap.add_argument("--receiver", default="receiver", help="Receiver executable filename/path (default: receiver)")
    ap.add_argument("--tester", default="Tester.py", help="Tester script filename/path (default: Tester.py)")
    ap.add_argument("--text", default="message.txt", help="Plaintext file (default: message.txt)")
    ap.add_argument("--cover", default=None, help="Optional cover wav (e.g. cover2.wav)")
    ap.add_argument("--outdir", default="runs", help="Output folder root (default: runs)")
    ap.add_argument("--timeout", type=int, default=300, help="Timeout seconds per command (default: 300)")
    ap.add_argument("--cases", default=None, help="Optional JSON file to override cases")
    args = ap.parse_args()

    # Make paths relative to this script's directory (fixes 'I ran it from another cwd' nonsense)
    root = Path(__file__).resolve().parent

    sender_path = (root / args.sender).resolve() if not Path(args.sender).is_absolute() else Path(args.sender)
    receiver_path = (root / args.receiver).resolve() if not Path(args.receiver).is_absolute() else Path(args.receiver)
    tester_path = (root / args.tester).resolve() if not Path(args.tester).is_absolute() else Path(args.tester)
    text_path = (root / args.text).resolve() if not Path(args.text).is_absolute() else Path(args.text)
    cover_path = None
    if args.cover:
        cover_path = (root / args.cover).resolve() if not Path(args.cover).is_absolute() else Path(args.cover)

    # Checks
    for p, name in [(sender_path, "sender"), (receiver_path, "receiver")]:
        if not p.exists():
            print(f"[ERR] {name} not found: {p}")
            sys.exit(2)
        if not os.access(p, os.X_OK):
            print(f"[ERR] {name} exists but is not executable: {p}")
            print("      Fix: chmod +x sender receiver")
            sys.exit(2)

    if not tester_path.exists():
        print(f"[ERR] Tester.py not found: {tester_path}")
        sys.exit(2)
    if not text_path.exists():
        print(f"[ERR] text file not found: {text_path}")
        sys.exit(2)
    if cover_path and not cover_path.exists():
        print(f"[ERR] cover wav not found: {cover_path}")
        sys.exit(2)

    plaintext = read_text_file(text_path)
    wc = word_count(plaintext)

    # Load/override cases if provided
    cases = DEFAULT_CASES
    if args.cases:
        cases_path = (root / args.cases).resolve() if not Path(args.cases).is_absolute() else Path(args.cases)
        data = json.loads(cases_path.read_text(encoding="utf-8"))
        cases = [Case(**c) for c in data["cases"]]

    run_id = datetime.now().strftime("%Y%m%d_%H%M%S")
    out_root = root / args.outdir / run_id
    ensure_dir(out_root)

    # 1) Run sender: encrypt + encode to encoded_signal.wav (as your C code does)
    sender_cmd = [str(sender_path), plaintext]
    if cover_path:
        sender_cmd.append(str(cover_path))

    rc, so, se = run_cmd(sender_cmd, cwd=root, timeout=args.timeout)
    if rc != 0:
        print("[ERR] sender failed. See sender_stderr.txt")
        sys.exit(3)

    base_wav = root / "encoded_signal.wav"
    if not base_wav.exists():
        print("[ERR] sender ran but encoded_signal.wav not found in script directory.")
        sys.exit(4)

    base_copy = out_root / "00_base_encoded_signal.wav"
    shutil.copy2(base_wav, base_copy)

    # 2) Receiver on base: sanity check
    rc_b, so_b, se_b = run_cmd([str(receiver_path), str(base_copy)], cwd=root, timeout=args.timeout)
    dec_base = parse_receiver_decrypted(so_b) if rc_b == 0 else ""

    def evaluate(case_name: str, wav_path: Path, receiver_rc: int, dec_text: str, receiver_stderr: str):
        dec_norm = normalize_text(dec_text)
        src_norm = normalize_text(plaintext)
        exact = (dec_norm == src_norm)
        sim = similarity_quick(plaintext, dec_text) if dec_text else 0.0
        return {
            "case": case_name,
            "wav": str(wav_path),
            "receiver_rc": receiver_rc,
            "exact_match": exact,
            "similarity": sim,
            "decrypted_len": len(dec_text),
            "word_count_decrypted": word_count(dec_text) if dec_text else 0,
            "receiver_stderr": receiver_stderr,
        }

    results = []
    results.append(evaluate("base", base_copy, rc_b, dec_base, se_b))

    # 3) Stress: apply Tester.py -> save WAV -> run receiver -> compare
    for idx, c in enumerate(cases, start=1):
        case_name = slug(f"{idx:02d}_{c.preset}_{c.noise}_snr{c.snr:g}_seed{c.seed}")
        stressed_wav = out_root / f"{case_name}.wav"

        tester_cmd = [
            sys.executable, str(tester_path),
            str(base_copy),
            str(stressed_wav),
            "--preset", c.preset,
            "--noise", c.noise,
            "--snr", str(c.snr),
            "--seed", str(c.seed),
        ]

        print(f"[*] Channel: {case_name}")
        rc_t, so_t, se_t = run_cmd(tester_cmd, cwd=root, timeout=args.timeout)

        if rc_t != 0 or not stressed_wav.exists():
            results.append({
                "case": case_name,
                "wav": str(stressed_wav),
                "receiver_rc": 999,
                "exact_match": False,
                "similarity": 0.0,
                "decrypted_len": 0,
                "word_count_decrypted": 0,
                "receiver_stderr": f"Tester failed (rc={rc_t}): {se_t}",
            })
            continue

        rc_r, so_r, se_r = run_cmd([str(receiver_path), str(stressed_wav)], cwd=root, timeout=args.timeout)

        dec = parse_receiver_decrypted(so_r) if rc_r == 0 else ""

        results.append(evaluate(case_name, stressed_wav, rc_r, dec, se_r))

    # 5) Print a quick pass/fail report
    for r in results:
        tag = "PASS ✅" if r["exact_match"] and r["receiver_rc"] == 0 else "FAIL ❌"
        print(f"{tag}  {r['case']:<35}  sim={r['similarity']:.4f}  rc={r['receiver_rc']}  wav={Path(r['wav']).name}")

    # Optional cleanup of root encoded_signal.wav (keep your workspace clean-ish)
    try:
        base_wav.unlink(missing_ok=True)
    except Exception:
        pass

if __name__ == "__main__":
    main()
