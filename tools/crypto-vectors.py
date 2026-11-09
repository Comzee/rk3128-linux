#!/usr/bin/env python3
"""Reproducible public fixtures: OpenSSL software ciphers and hashlib hashes.

No frame needed. Output is bounded to 20 MiB; inputs are generated, never user media.
OpenSSL 3's legacy provider supplies single DES. Python cryptography supplies
three independent DES stages for EEE (its TripleDES with one repeated key is DES).
This is test reference code only.
"""
import argparse
import hashlib
import subprocess


def enc(name, key, iv, data):
    cmd = ["openssl", "enc", "-" + name, "-nopad", "-nosalt", "-K", key.hex(),
           "-provider", "default", "-provider", "legacy"]
    if iv:
        cmd += ["-iv", iv.hex()]
    return subprocess.run(cmd, input=data, capture_output=True, check=True,
                          timeout=10).stdout


def eee_enc(key, iv, data):
    from cryptography.hazmat.decrepit.ciphers.algorithms import TripleDES
    from cryptography.hazmat.primitives.ciphers import Cipher, modes
    stages = [Cipher(TripleDES(key[i:i + 8]), modes.ECB()).encryptor() for i in (0, 8, 16)]
    if not iv:
        for stage in stages:
            data = stage.update(data) + stage.finalize()
        return data
    result = bytearray()
    for i in range(0, len(data), 8):
        block = bytes(x ^ y for x, y in zip(data[i:i + 8], iv))
        for stage in stages:
            block = stage.update(block)
        result.extend(block)
        iv = block
    for stage in stages:
        assert not stage.finalize()
    return bytes(result)


def fixtures():
    # FIPS 197 AES-128 example and the classic DES known-answer vector.
    key = bytes(range(16))
    plain = bytes.fromhex("00112233445566778899aabbccddeeff")
    assert enc("aes-128-ecb", key, b"", plain).hex() == "69c4e0d86a7b0430d8cdb78070b4c55a"
    assert enc("des-ecb", bytes.fromhex("133457799bbcdff1"), b"",
               bytes.fromhex("0123456789abcdef")).hex() == "85e813540f0ab405"
    yield "skcipher", "ecb(aes)", "ecb-aes-rk3128", key, b"", plain, enc("aes-128-ecb", key, b"", plain)
    yield "skcipher", "ecb(des)", "ecb-des-rk3128", bytes.fromhex("133457799bbcdff1"), b"", bytes.fromhex("0123456789abcdef"), bytes.fromhex("85e813540f0ab405")
    for cipher, keylens in (("aes", (16, 24, 32)), ("des", (8,)), ("des3_ede", (24,)), ("des3_eee", (24,))):
        for mode in ("ecb", "cbc", "ctr"):
            if mode == "ctr" and cipher != "aes":
                continue
            block = 16 if cipher == "aes" else 8
            for kl in keylens:
                key = bytes((i * 19 + 7) & 255 for i in range(kl))
                iv = bytes((i * 13 + 5) & 255 for i in range(block)) if mode != "ecb" else b""
                openssl = f"aes-{kl * 8}-{mode}" if cipher == "aes" else f"des-{mode}" if cipher == "des" else f"des-ede3-{mode}"
                for n in (0, 1, 7, 8, 15, 16, 17, 31, 32, 63, 64, 65, 256, 4096, 16384, 16385, 32768, 65536):
                    if mode != "ctr" and n % block:
                        continue
                    plain = bytes((i * 73 + 19 + n) & 255 for i in range(n))
                    expected = eee_enc(key, iv, plain) if cipher == "des3_eee" else enc(openssl, key, iv, plain)
                    yield "skcipher", f"{mode}({cipher})", f"{mode}-{cipher.replace('_', '-')}-rk3128", key, iv, plain, expected
    for alg in ("md5", "sha1", "sha256"):
        for n in (0, 1, 2, 3, 4, 7, 8, 15, 16, 17, 31, 32, 55, 56, 63, 64, 65, 127, 128, 129, 256, 4096, 16384, 16385, 32768, 65536):
            plain = bytes((i * 73 + 19 + n) & 255 for i in range(n))
            yield "hash", alg, alg + "-rk3128", b"", b"", plain, hashlib.new(alg, plain).digest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("output")
    ap.add_argument("--software", action="store_true",
                    help="AF_ALG software baseline: omit EEE, which has no software kernel implementation")
    a = ap.parse_args()
    count = size = 0
    with open(a.output, "w", encoding="ascii") as f:
        for fields in fixtures():
            if a.software and "des3_eee" in fields[1]:
                continue
            line = " ".join(x.hex() or "-" if isinstance(x, bytes) else x for x in fields) + "\n"
            size += len(line)
            if size > 20 * 1024 * 1024:
                raise ValueError("fixture size limit")
            f.write(line)
            count += 1
    print(f"PASS generated {count} vectors, {size} bytes; OpenSSL KATs match")


if __name__ == "__main__":
    main()
