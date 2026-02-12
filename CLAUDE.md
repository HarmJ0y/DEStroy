# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

DEStroy is a GPU-accelerated tool for recovering DES keys from NetNTLMv1 authentication hashes using rainbow tables. Derived from [rainbowcrackalack](https://github.com/jtesta/rainbowcrackalack) by Joe Testa. Licensed under GPL-3.0.

The tool takes 8-byte ciphertexts (from NetNTLMv1 responses using the hardcoded challenge `1122334455667788`) and searches rainbow tables for the 7-byte DES key. Rainbow tables only work with this specific challenge (use Responder with `--lm` to force it).

## Build Commands

```bash
make                # Build all Linux targets
make clean          # Remove all binaries
make windows        # Cross-compile for Windows (requires mingw)
```

Linux builds use `gcc` with `-std=gnu99 -O2`. Windows native builds use MSVC from an x64 Native Tools Command Prompt. No test suite exists.

## Build Targets

Five binaries are produced on Linux: `gpu_lookup`, `precompute`, `candidate_lookup`, `candidate_check`, `destroy_optimized`.

## Architecture

### Three-Stage Pipeline

```
Ciphertext (8 bytes)
    → [Precompute] (GPU)  → 881,688 endpoints
    → [Lookup] (CPU)      → candidate (start_index, position) pairs
    → [Verify] (GPU)      → 7-byte DES key
```

Each stage has a standalone CLI tool (`precompute`, `candidate_lookup`, `candidate_check`) and all three are integrated into `destroy_optimized` as a streaming pipeline with multi-GPU support.

### Source Organization

- **Entry points** (`src/*_main.c`): Each binary has its own main. `destroy_main.c` (~1630 lines) is the most complex, implementing a concurrent streaming pipeline with batched candidate queues.
- **Core crypto** (`src/des.c`, `src/netntlmv1.c`): CPU-side DES and key expansion. The GPU kernels have their own independent DES implementations.
- **Rainbow table logic** (`src/rainbow.c`, `src/table.c`): Chain reduction functions and table file loading (mmap on Linux, malloc+fread on Windows).
- **GPU layer** (`src/opencl_host.c`, `src/opencl_dyn.c`): OpenCL context/kernel management. `opencl_dyn.c` dynamically loads the OpenCL library at runtime via dlsym/GetProcAddress.
- **Platform abstraction** (`src/platform.c`): Directory iteration and dynamic library loading across Linux/Windows.

### OpenCL Kernels

Two kernel files in `kernels/`:
- **`precompute.cl`**: Generates endpoints. Includes chunked variants (`precompute_init`/`precompute_step`) for memory-constrained GPUs.
- **`false_alarm.cl`**: Verifies candidates by regenerating chains. Uses `atomic_cmpxchg` for thread-safe winner selection.

Both kernels contain full DES implementations (S-boxes, key schedule, encryption) and the hardcoded challenge after DES initial permutation (`X = 0xf0aaf0aa; Y = 0x00cd00cd`).

### Key Constants (in `include/utils.h`)

- `CHAIN_LEN`: 881,689 steps per rainbow chain
- `CHARSET_LEN`: 256 (full byte range)
- `PLAINTEXT_LEN_MAX`: 7 (DES key size)
- `REDUCTION_OFFSET`: 0

### Rainbow Table Format

Binary files containing sorted `(start_index, end_index)` pairs as `uint64_t` values. Tables are searched via merge search (both endpoints and table entries are sorted).

## Dependencies

- OpenCL 1.2 runtime (dynamically loaded, no link-time dependency)
- pthreads (Linux, for `candidate_lookup` and `destroy_optimized`)
- Standard C library only; no external package manager dependencies
