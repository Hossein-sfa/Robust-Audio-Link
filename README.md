Robust Audio Link

Secure Text Transmission over Audio using AES-256-CTR and Robust Phone-Band BFSK

Overview

PhonoCrypt is a C-based implementation of a secure audio communication system developed for a university cryptography project. The system encrypts textual messages using AES-256 in CTR mode and transmits them over an audio channel using robust phone-band Binary Frequency Shift Keying (BFSK).

The project integrates cryptography, digital signal processing, modulation techniques, and audio steganography into a complete sender/receiver architecture. It demonstrates how encrypted data can be reliably transmitted through acoustic channels.

⸻

Phase 1 – Secure Audio Transmission

Sender Pipeline:
	1.	Plaintext input (minimum 250 words)
	2.	AES-256-CTR encryption
	3.	Framing structure:
	•	Magic header: “STEG”
	•	32-bit payload length (big-endian)
	•	Ciphertext
	•	CRC32 checksum
	4.	Repetition coding (REP = 3)
	5.	BFSK modulation:
	•	1200 Hz → bit 0
	•	2200 Hz → bit 1
	•	15 ms per bit
	6.	1.5-second synchronization preamble
	7.	Output: 44.1 kHz mono WAV file

Receiver Pipeline:
	1.	Load WAV file (automatic mono conversion)
	2.	DC offset removal
	3.	RMS normalization
	4.	Band-pass filtering (700–2600 Hz)
	5.	Preamble detection
	6.	Boundary refinement using “STEG” header
	7.	Phase-robust I/Q energy detection
	8.	Majority vote decoding (REP = 3)
	9.	CRC32 validation
	10.	AES-256-CTR decryption
	11.	Plaintext recovery

⸻

Phase 2 – Audio Steganography

In the second phase, the encrypted BFSK signal is embedded into a meaningful audio cover (music or speech).

Output signal is generated as:

Output = Cover × Gain + BFSK × StegoStrength

Design priorities:
	•	Robustness (primary objective)
	•	Capacity (≤ 10 minutes output for 250-word message)
	•	Imperceptibility (audio remains natural)

⸻

Robustness Techniques Implemented
	•	Phone-band BFSK (1200/2200 Hz)
	•	Synchronization preamble
	•	Phase-independent I/Q energy detection
	•	Band-pass filtering before demodulation
	•	Repetition coding (REP = 3)
	•	CRC32 integrity verification
	•	RMS normalization
	•	Boundary refinement using known header

These mechanisms improve reliability under additive noise, acoustic playback/record paths, moderate compression, and channel distortions.

⸻

Requirements
	•	C compiler (GCC or Clang)
	•	OpenSSL
	•	libsndfile
	•	Math library

⸻

Build

Compile sender:
gcc sender.c -o sender -lsndfile -lssl -lcrypto -lm

Compile receiver:
gcc receiver.c -o receiver -lsndfile -lssl -lcrypto -lm

⸻

Usage

Encode (pure BFSK):
./sender “Your message here”

Encode with cover audio:
./sender “Your message here” cover.wav

This generates:
encoded_signal.wav

Decode:
./receiver encoded_signal.wav

The original plaintext will be printed after successful CRC verification and decryption.

⸻

Educational Context

This project demonstrates practical implementation of:
	•	Symmetric cryptography (AES-CTR)
	•	Stream-based encryption
	•	Digital modulation (BFSK)
	•	Audio signal processing
	•	Basic channel coding
	•	Audio steganography trade-offs

Developed as part of a university cryptography course.

⸻

Limitations
	•	Fixed key and IV (demonstration only)
	•	Basic repetition coding (no advanced FEC)
	•	No adaptive timing recovery
	•	Not resistant to strong AI-based noise suppression
