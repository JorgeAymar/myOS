#!/usr/bin/env python3
"""Generate kernel/net/ca_roots.c (BearSSL trust anchors) from a PEM CA bundle.

Usage: python3 -I tools/gen_ca_roots.py cacert.pem > kernel/net/ca_roots.c
Bundle source: https://curl.se/ca/cacert.pem (Mozilla roots). Stdlib only.
Keeps RSA and EC (P-256/384/521) roots; others are skipped (summary on stderr).
"""
import base64, hashlib, re, sys

EC_CURVES = {
    bytes.fromhex("2a8648ce3d030107"): "BR_EC_secp256r1",
    bytes.fromhex("2b81040022"): "BR_EC_secp384r1",
    bytes.fromhex("2b81040023"): "BR_EC_secp521r1",
}
OID_RSA = bytes.fromhex("2a864886f70d010101")
OID_EC = bytes.fromhex("2a8648ce3d0201")


def tlv(buf, pos):
    """Return (tag, value_start, end) for the TLV at buf[pos]."""
    tag = buf[pos]
    l = buf[pos + 1]
    p = pos + 2
    if l & 0x80:
        n = l & 0x7F
        l = int.from_bytes(buf[p:p + n], "big")
        p += n
    return tag, p, p + l


def children(buf, start, end):
    """List of (tag, tlv_start, value_start, value_end)."""
    out = []
    while start < end:
        tag, vs, ve = tlv(buf, start)
        out.append((tag, start, vs, ve))
        start = ve
    return out


def parse_cert(der):
    """Return (subject Name TLV bytes, key info) or raise ValueError."""
    _, vs, ve = tlv(der, 0)
    tbs = children(der, vs, ve)[0]
    items = children(der, tbs[2], tbs[3])
    i = 1 if items[0][0] == 0xA0 else 0  # optional explicit version
    # serial, sigalg, issuer, validity, subject, spki
    subj = items[i + 4]
    spki = items[i + 5]
    name = der[subj[1]:subj[3]]
    alg, bits = children(der, spki[2], spki[3])[:2]
    algp = children(der, alg[2], alg[3])
    oid = der[algp[0][2]:algp[0][3]]
    key = der[bits[2] + 1:bits[3]]  # skip unused-bits byte
    if oid == OID_RSA:
        _, ks, ke = tlv(key, 0)
        n, e = children(key, ks, ke)[:2]
        return name, ("rsa", key[n[2]:n[3]].lstrip(b"\0"), key[e[2]:e[3]].lstrip(b"\0"))
    if oid == OID_EC:
        if len(algp) < 2 or algp[1][0] != 0x06:
            raise ValueError("EC without named curve")
        cv = der[algp[1][2]:algp[1][3]]
        if cv not in EC_CURVES:
            raise ValueError("unsupported EC curve " + cv.hex())
        if key[0] != 4:
            raise ValueError("compressed EC point")
        return name, ("ec", EC_CURVES[cv], key)
    raise ValueError("unsupported key algorithm " + oid.hex())


def carr(name, data):
    lines = []
    for i in range(0, len(data), 12):
        lines.append("\t" + ", ".join("0x%02x" % b for b in data[i:i + 12]) + ",")
    return "static const unsigned char %s[] = {\n%s\n};\n" % (name, "\n".join(lines))


def main():
    raw = open(sys.argv[1], "rb").read()
    sha = hashlib.sha256(raw).hexdigest()
    text = raw.decode("ascii", "replace")
    m = re.search(r"Certificate data from Mozilla as of:.*", text)
    dateline = m.group(0).strip() if m else "(no date line)"
    blocks = re.finditer(
        r"^([^\n]+)\n={3,}\n.*?-----BEGIN CERTIFICATE-----\n(.*?)-----END CERTIFICATE-----",
        text, re.S | re.M)
    body, table, skips = [], [], []
    kept = skipped = 0
    for b in blocks:
        label = b.group(1).strip()
        der = base64.b64decode("".join(b.group(2).split()))
        try:
            name, key = parse_cert(der)
        except (ValueError, IndexError) as ex:
            skipped += 1
            skips.append("%s: %s" % (label, ex))
            continue
        idx = kept
        kept += 1
        body.append("/* %d: %s */" % (idx, label.replace("*/", "* /")))
        body.append(carr("ta%d_dn" % idx, name))
        if key[0] == "rsa":
            body.append(carr("ta%d_n" % idx, key[1]))
            body.append(carr("ta%d_e" % idx, key[2]))
            kt = "BR_KEYTYPE_RSA"
            kv = ("{ .rsa = { (unsigned char *)ta%d_n, sizeof ta%d_n, "
                  "(unsigned char *)ta%d_e, sizeof ta%d_e } }" % (idx, idx, idx, idx))
        else:
            body.append(carr("ta%d_q" % idx, key[2]))
            kt = "BR_KEYTYPE_EC"
            kv = ("{ .ec = { %s, (unsigned char *)ta%d_q, sizeof ta%d_q } }"
                  % (key[1], idx, idx))
        table.append("\t{ { (unsigned char *)ta%d_dn, sizeof ta%d_dn }, BR_X509_TA_CA,\n"
                     "\t  { %s, %s } }," % (idx, idx, kt, kv))
    out = ["""/*
 * GENERATED FILE - do not edit by hand.
 * Mozilla root CA trust anchors for BearSSL (br_x509_trust_anchor).
 * Source : https://curl.se/ca/cacert.pem
 * Bundle : %s
 * SHA-256: %s
 * Roots  : %d kept (RSA, EC P-256/384/521), %d skipped
 * Regen  : curl -o /tmp/cacert.pem https://curl.se/ca/cacert.pem &&
 *          python3 -I tools/gen_ca_roots.py /tmp/cacert.pem > kernel/net/ca_roots.c
 */
#include <stddef.h>
#include <bearssl_x509.h>
#include "ca_roots.h"
""" % (dateline, sha, kept, skipped)]
    out.extend(body)
    out.append("const br_x509_trust_anchor tls_TAs[] = {")
    out.extend(table)
    out.append("};\n\nconst size_t tls_TAs_NUM = %d;" % kept)
    sys.stdout.write("\n".join(out) + "\n")
    sys.stderr.write("ca roots: %d kept, %d skipped\n" % (kept, skipped))
    for s in skips:
        sys.stderr.write("  skipped %s\n" % s)


main()
