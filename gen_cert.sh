#!/bin/bash
# Generate self-signed ECDSA P-256 cert with 14-day validity
# Outputs cert_data.h with embedded DER arrays
set -e

OUTDIR="${1:-.}"

# Generate key
openssl ecparam -name prime256v1 -genkey -noout -out "$OUTDIR/server.key"

# Generate self-signed cert (14 days)
openssl req -new -x509 -key "$OUTDIR/server.key" \
  -out "$OUTDIR/server.crt" \
  -days 14 \
  -subj "/CN=QUIC Echo Server/O=stare-socket"

# Convert to DER
openssl x509 -in "$OUTDIR/server.crt" -outform DER -out "$OUTDIR/server.crt.der"
# Convert key to PKCS8 DER (wolfSSL prefers this format)
openssl pkcs8 -topk8 -nocrypt -in "$OUTDIR/server.key" -outform DER -out "$OUTDIR/server.key.der"

# Print SHA-256 hash (for WebTransport serverCertificateHashes)
echo ""
echo "=== Certificate SHA-256 hash ==="
HASH_HEX=$(openssl x509 -in "$OUTDIR/server.crt" -outform DER | openssl dgst -sha256 | awk '{print $NF}')
HASH_B64=$(openssl x509 -in "$OUTDIR/server.crt" -outform DER | openssl dgst -sha256 -binary | openssl base64 -A)
echo "Hex: $HASH_HEX"
echo "Base64: $HASH_B64"

# Generate C header with embedded DER data
python3 -c "
import sys

def to_c_array(name, data):
    lines = []
    lines.append(f'static const unsigned char {name}[] = {{')
    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        hex_str = ', '.join(f'0x{b:02x}' for b in chunk)
        lines.append(f'    {hex_str},')
    lines.append('};')
    lines.append(f'static const int {name}_len = sizeof({name});')
    return '\n'.join(lines)

with open('$OUTDIR/server.crt.der', 'rb') as f:
    cert_der = f.read()
with open('$OUTDIR/server.key.der', 'rb') as f:
    key_der = f.read()

# Compute SHA-256
import hashlib
h = hashlib.sha256(cert_der).digest()
hash_hex = h.hex()
import base64
hash_b64 = base64.b64encode(h).decode()

print('/* Auto-generated — do not edit */')
print(f'/* Certificate SHA-256 (hex): {hash_hex} */')
print(f'/* Certificate SHA-256 (base64): {hash_b64} */')
print()
print(to_c_array('cert_der', cert_der))
print()
print(to_c_array('key_der', key_der))
" > "$OUTDIR/cert_data.h"

echo "Generated cert_data.h ($(wc -c < "$OUTDIR/cert_data.h") bytes)"

# Cleanup temp files
rm -f "$OUTDIR/server.key" "$OUTDIR/server.crt" "$OUTDIR/server.crt.der" "$OUTDIR/server.key.der"
