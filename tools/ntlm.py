#!/usr/bin/env python3
"""Generate NetNTLMv1 challenge/response pairs for a given password."""

import sys
from binascii import hexlify
from Crypto.Hash import MD4


def ntlm_hash(password: str) -> bytes:
    """Compute the NTLM (MD4) hash of a password."""
    h = MD4.new()
    h.update(password.encode("utf-16-le"))
    return h.digest()


def des_expand_key(key_7: bytes) -> bytes:
    """Expand a 7-byte key into an 8-byte DES key with parity bits."""
    k = int.from_bytes(key_7, "big")
    # Split 56 bits into 8 groups of 7, inserting a parity bit for each
    expanded = []
    for i in range(7, -1, -1):
        b = (k >> (i * 7)) & 0x7F
        # Shift left 1 to make room for parity bit
        b = b << 1
        # Set odd parity
        parity = bin(b).count("1") % 2
        b |= (1 - parity)
        expanded.append(b)
    return bytes(expanded)


def des_encrypt_block(key_7: bytes, plaintext: bytes) -> bytes:
    """DES-encrypt an 8-byte block using a 7-byte key."""
    from Crypto.Cipher import DES
    key_8 = des_expand_key(key_7)
    cipher = DES.new(key_8, DES.MODE_ECB)
    return cipher.encrypt(plaintext)


def lm_hash(password: str) -> bytes:
    """Compute the LM hash of a password."""
    LM_MAGIC = b"KGS!@#$%"
    # Uppercase, encode to OEM (ASCII), pad/truncate to 14 bytes
    pw_upper = password.upper().encode("ascii", errors="ignore")[:14]
    pw_upper = pw_upper.ljust(14, b"\x00")

    # Split into two 7-byte halves, each DES-encrypts the magic string
    return des_encrypt_block(pw_upper[0:7], LM_MAGIC) + \
           des_encrypt_block(pw_upper[7:14], LM_MAGIC)


def compute_response(hash_16: bytes, challenge: bytes) -> bytes:
    """Compute a 24-byte NetNTLMv1 response from a 16-byte hash + 8-byte challenge."""
    # Pad hash (16 bytes) to 21 bytes with nulls
    padded = hash_16 + b"\x00" * 5

    # Split into three 7-byte DES keys, each encrypts the challenge
    return des_encrypt_block(padded[0:7], challenge) + \
           des_encrypt_block(padded[7:14], challenge) + \
           des_encrypt_block(padded[14:21], challenge)


def format_netntlmv1(username: str, domain: str, password: str,
                     challenge_hex: str = "1122334455667788") -> str:
    """Generate a full NetNTLMv1 hash string."""
    challenge = bytes.fromhex(challenge_hex)
    nt_h = ntlm_hash(password)
    lm_h = lm_hash(password)

    lm_resp = compute_response(lm_h, challenge)
    nt_resp = compute_response(nt_h, challenge)

    lm_resp_hex = hexlify(lm_resp).decode().upper()
    nt_resp_hex = hexlify(nt_resp).decode().upper()

    # Format: username::domain:LMresponse:NTresponse:challenge
    return f"{username}::{domain}:{lm_resp_hex}:{nt_resp_hex}:{challenge_hex}"


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <password> [username] [domain]")
        print(f"       {sys.argv[0]} --batch <file>  (file has password per line)")
        sys.exit(1)

    if sys.argv[1] == "--batch":
        if len(sys.argv) < 3:
            print("Provide a file with one password per line")
            sys.exit(1)
        with open(sys.argv[2]) as f:
            for i, line in enumerate(f):
                pw = line.rstrip("\n")
                if not pw:
                    continue
                username = f"user{i}"
                domain = "WORKGROUP"
                nt_hash = ntlm_hash(pw)
                result = format_netntlmv1(username, domain, pw)
                print(f"# password={pw}  NTLM={hexlify(nt_hash).decode()}")
                print(result)
                print()
    else:
        password = sys.argv[1]
        username = sys.argv[2] if len(sys.argv) > 2 else "hashcat"
        domain = sys.argv[3] if len(sys.argv) > 3 else "DUSTIN-5AA37877"

        nt_hash = ntlm_hash(password)
        result = format_netntlmv1(username, domain, password)

        print(f"Password:  {password}")
        print(f"NTLM hash: {hexlify(nt_hash).decode()}")
        print(f"")
        print(result)


if __name__ == "__main__":
    main()
