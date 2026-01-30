# DEStroy 

A GPU-accelerated tool for recovering DES keys from NetNTLMv1 authentication hashes using rainbow tables.

## Credits

This project is based on [rainbowcrackalack](https://github.com/jtesta/rainbowcrackalack) by Joe Testa, a GPU-accelerated rainbow table generator and lookup tool.

## License

This project is licensed under the GNU General Public License v3.0 (GPL-3.0), consistent with the rainbowcrackalack codebase it derives from.

See [LICENSE](LICENSE) for the full license text, or visit https://www.gnu.org/licenses/gpl-3.0.html

---

## Quick Start
```bash
# Linux
make

# Single ciphertext lookup
./precompute 535549550D915078 working/535549550D915078.endpoints
./candidate_lookup working/535549550D915078.endpoints /path/to/tables -o working/535549550D915078.candidates
./candidate_check 535549550D915078 working/535549550D915078.candidates -o working/535549550D915078.result
```

## Overview

NetNTLMv1 uses DES encryption internally. This tool:
1. Takes an 8-byte ciphertext (one third of a NetNTLMv1 response)
2. Searches rainbow tables for the 7-byte DES key that produced it
3. Uses GPU acceleration for both precomputation and false alarm checking

### Pipeline
```
┌─────────────┐     ┌─────────────┐     ┌─────────────┐
│  Precompute │ --> │   Lookup    │ --> │    Check    │
│    (GPU)    │     │   (CPU)     │     │    (GPU)    │
└─────────────┘     └─────────────┘     └─────────────┘
   .endpoints         .candidates          .result
```

1. **Precompute**: Generate rainbow chain endpoints from ciphertext (GPU)
2. **Lookup**: Search rainbow tables for matching endpoints (CPU)
3. **Check**: Verify candidates and recover DES key (GPU)

---

## How NetNTLMv1 Works
```
User password → NTLM hash (16 bytes)

NTLM hash split into 3 parts:
  - Bytes 0-6   → DES key 1 (7 bytes)
  - Bytes 7-13  → DES key 2 (7 bytes)  
  - Bytes 14-15 + 5 null bytes → DES key 3 (7 bytes)

Each 7-byte key is expanded to 8 bytes (parity bits added)
Each DES key encrypts the SERVER CHALLENGE (8 bytes)

Result: 3 × 8-byte ciphertexts = 24-byte NetNTLMv1 response
```

**Real Example:**
```
Password:             Password123
NTLM hash (MD4):      58A478135A93AC3BF058A5EA0E8FDB71
Challenge:            1122334455667788

Split NTLM hash into DES keys:
  Key1: 58A478135A93AC     (bytes 0-6)
  Key2: 3BF058A5EA0E8F     (bytes 7-13)
  Key3: DB71000000000000   (bytes 14-15 + padding)

DES encrypt challenge with each key:
  DES(Key1, challenge) = CT1 = 535549550D915078
  DES(Key2, challenge) = CT2 = B4F2F4334C1992E0
  DES(Key3, challenge) = CT3 = 0B3693107DC5A855

NetNTLMv1 response:   535549550D915078B4F2F4334C1992E00B3693107DC5A855
```

**Responder format:**
```
user::domain:535549550D915078B4F2F4334C1992E00B3693107DC5A855:1122334455667788
```

To recover the NTLM hash, run this tool 3 times (once per ciphertext), then concatenate the results.

---

## Hardcoded Challenge

> **IMPORTANT:** These rainbow tables only work with challenge `1122334455667788`.

This challenge is hardcoded in two kernel files:

**`kernels/precompute.cl`** and **`kernels/false_alarm.cl`**:
```c
// Challenge 1122334455667788 after DES initial permutation
uint X = 0xf0aaf0aa;
uint Y = 0x00cd00cd;
```

Use [Responder](https://github.com/lgandx/Responder) with `--lm` flag to force this challenge.

---

## Build

### Linux
```bash
make
```

### Windows (x64 Native Tools Command Prompt)
```bash
make
```

### Cross-compile for Windows (from Linux)
```bash
make windows
```

---

## CLI Tools

### precompute

Generate rainbow chain endpoints from a ciphertext (GPU).
```bash
./precompute <ciphertext_hex> <output_file> [-g gpu_index]
```

### candidate_lookup

Search rainbow tables for matching endpoints (CPU).
```bash
./candidate_lookup <endpoints_file> <table_dir> [...] [-o output_file]
```

### candidate_check

Verify candidates and recover the DES key (GPU).
```bash
./candidate_check <ciphertext_hex> <candidates_file> [-o output_file] [-g gpu_index]
```

### gpu_lookup (legacy)

All-in-one lookup tool.
```bash
./gpu_lookup <table_dir> <ciphertext_hex>
```

---

## How Rainbow Tables Work

### The Problem

- 7-byte keyspace = 256^7 = 72 quadrillion possible keys
- Storing all (key → ciphertext) pairs = 1.1 exabytes
- Impossible to store

### The Solution: Rainbow Chains

A rainbow table stores compressed chains:
```
start_index → key₀ → hash₀ → reduce₀ → 
              key₁ → hash₁ → reduce₁ → 
              ...
              key_n → hash_n → reduce_n → end_index

Only store: (start_index, end_index)
Chain length: 881,689 steps
```

### Lookup Algorithm

1. **Precompute End Indices (GPU)** - Given target ciphertext, compute where it would land at each chain position
2. **Binary Search Tables (CPU)** - Search sorted end indices for matches
3. **Verify Candidates (GPU)** - Regenerate chains to confirm matches and extract keys


---

## Project Structure
```
DEStroy/
├── Makefile
├── README.md
├── LICENSE
├── dep/
│   └── CL/
├── kernels/
│   ├── precompute.cl
│   └── false_alarm.cl
├── include/
│   ├── platform.h
│   ├── utils.h
│   ├── des.h
│   ├── rainbow.h
│   ├── table.h
│   ├── opencl_dyn.h
│   └── opencl_host.h
└── src/
    ├── main.c
    ├── precompute_main.c
    ├── candidate_lookup_main.c
    ├── candidate_check_main.c
    ├── platform.c
    ├── utils.c
    ├── des.c
    ├── rainbow.c
    ├── table.c
    ├── opencl_dyn.c
    └── opencl_host.c
```