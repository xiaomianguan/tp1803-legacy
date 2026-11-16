#!/usr/bin/env python3
"""Merge root CAs into an Android (libcore) cacerts directory.

usage: merge-cacerts.py <cacerts_dir> <cert files (PEM bundles or DER/PEM singles)...>

Files are named <subject_hash_old>.<n> and contain the PEM followed by
`openssl x509 -noout -text -fingerprint` output, matching AOSP's format.
Certificates already present (same SHA-1 of the DER) are skipped; expired
certificates are skipped.
"""
import base64, hashlib, os, re, subprocess, sys

PEM_RE = re.compile(rb"-----BEGIN CERTIFICATE-----\s+(.+?)\s+-----END CERTIFICATE-----", re.S)


def ders_from_file(path):
    data = open(path, "rb").read()
    blocks = PEM_RE.findall(data)
    if blocks:
        return [base64.b64decode(b"".join(b.split())) for b in blocks]
    return [data]  # single DER certificate


def ossl(args, inp):
    return subprocess.run(["openssl", "x509"] + args, input=inp, capture_output=True, check=True).stdout


def main():
    store, inputs = sys.argv[1], sys.argv[2:]
    have = set()
    for name in os.listdir(store):
        for der in ders_from_file(os.path.join(store, name)):
            have.add(hashlib.sha1(der).hexdigest())

    added = skipped_dup = skipped_bad = 0
    for path in inputs:
        for der in ders_from_file(path):
            fp = hashlib.sha1(der).hexdigest()
            if fp in have:
                skipped_dup += 1
                continue
            try:
                # -checkend 0 exits non-zero if the certificate has expired
                subprocess.run(["openssl", "x509", "-inform", "DER", "-noout", "-checkend", "0"],
                               input=der, capture_output=True, check=True)
                h = ossl(["-inform", "DER", "-noout", "-subject_hash_old"], der).decode().strip()
                pem = ossl(["-inform", "DER", "-outform", "PEM"], der)
                text = ossl(["-inform", "DER", "-noout", "-text", "-fingerprint", "-sha1"], der)
            except subprocess.CalledProcessError:
                skipped_bad += 1
                continue
            n = 0
            while os.path.exists(os.path.join(store, "%s.%d" % (h, n))):
                n += 1
            out = os.path.join(store, "%s.%d" % (h, n))
            with open(out, "wb") as f:
                f.write(pem + text.replace(b"sha1 Fingerprint=", b"SHA1 Fingerprint="))
            have.add(fp)
            added += 1
            subj = ossl(["-inform", "DER", "-noout", "-subject"], der).decode().strip()
            print("added %s  %s" % (os.path.basename(out), subj))
    print("added=%d already-present=%d expired/unparsable=%d" % (added, skipped_dup, skipped_bad))


if __name__ == "__main__":
    main()
