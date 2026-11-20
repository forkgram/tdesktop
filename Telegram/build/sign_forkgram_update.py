#!/usr/bin/env python3

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import time

HEADER = struct.Struct('<4sIBBBQQ')
U32 = struct.Struct('<I')
TARGETS = {
    'win-x86': (0, 0),
    'win-x64': (0, 1),
    'win-arm': (0, 2),
    'linux-x64': (2, 1),
}
KEY_ID = 'fg-2026a'
MAX_PAYLOAD_SIZE = 256 * 1024 * 1024


def field(data):
    return U32.pack(len(data)) + data


def sign_update(release_dir, version_file, keys, platform, private_key):
    version = dict(line.split() for line in version_file.read_text().splitlines() if line.strip())
    app_version = int(version['AppVersion'])
    beta = version['BetaChannel'] != '0'
    channel = 'beta' if beta else 'stable'
    suffix = '-beta' if beta else ''
    destination = release_dir / f'td-update-{platform}-{app_version}{suffix}'
    unsigned = destination.with_suffix('.unsigned')

    manifest_file = keys / 'manifest.min.json'
    signature_file = keys / 'manifest.sig'
    manifest = manifest_file.read_bytes()
    manifest_signature = signature_file.read_bytes()
    subprocess.run([
        'openssl', 'pkeyutl', '-verify', '-pubin', '-inkey', str(keys / 'root-public.pem'),
        '-rawin', '-in', str(manifest_file), '-sigfile', str(signature_file),
    ], check=True)

    policy = json.loads(manifest)
    key = next(entry for entry in policy['keys'] if entry['id'] == KEY_ID)
    expires = key.get('expires')
    if (policy['channels'][channel] != [[KEY_ID]] or KEY_ID in policy['revoked']
            or key['alg'] != 'Ed25519' or (expires and expires <= time.time())):
        raise ValueError('The release key must currently authorize this channel')
    public_key = subprocess.run(
        ['openssl', 'pkey', '-pubout', '-outform', 'DER'],
        input=private_key, check=True, stdout=subprocess.PIPE,
    ).stdout
    expected_key = base64.urlsafe_b64decode(key['x'] + '=' * (-len(key['x']) % 4))
    if public_key != bytes.fromhex('302a300506032b6570032100') + expected_key:
        raise ValueError('The signing key does not match the release manifest')

    with unsigned.open('rb') as source:
        header = source.read(HEADER.size)
        magic, format_version, channel_id, os_id, arch, packed_version, _ = HEADER.unpack(header)
        if (magic, format_version, channel_id, os_id, arch, packed_version) != (
                b'TDUP', 2, int(beta), *TARGETS[platform], app_version << 32):
            raise ValueError('The unsigned package targets a different release')
        manifest_block = field(manifest)
        signed_region = header + manifest_block
        root_signature = field(manifest_signature)
        unsigned_metadata = manifest_block + root_signature + U32.pack(0)
        if source.read(len(unsigned_metadata)) != unsigned_metadata:
            raise ValueError('Expected the checked-in manifest and no update signatures')
        payload_size, = U32.unpack(source.read(U32.size))
        if not 0 < payload_size <= MAX_PAYLOAD_SIZE:
            raise ValueError('Invalid unsigned update payload size')
        payload = source.read(payload_size)
        if len(payload) != payload_size or source.read(1):
            raise ValueError('Unsigned update payload length does not match the header')

    with tempfile.TemporaryDirectory() as tmp:
        message = Path(tmp) / 'signing-input.bin'
        message.write_bytes(signed_region + hashlib.sha256(payload).digest())
        signature = subprocess.run([
            'openssl', 'pkeyutl', '-sign', '-inkey', '/dev/stdin', '-rawin', '-in', str(message),
        ], input=private_key, check=True, stdout=subprocess.PIPE).stdout
    if len(signature) != 64:
        raise ValueError('Unexpected Ed25519 signature size')

    with destination.open('xb') as output:
        output.write(signed_region + root_signature + U32.pack(1))
        output.write(field(KEY_ID.encode('ascii')) + field(signature))
        output.write(U32.pack(payload_size))
        output.write(payload)
    unsigned.unlink()
    print(f'Signed {destination.name}')


def main():
    parser = argparse.ArgumentParser(description='Sign a Forkgram release update with the Ed25519 release key.')
    parser.add_argument('release_dir', type=Path)
    parser.add_argument('version_file', type=Path)
    parser.add_argument('keys', type=Path)
    parser.add_argument('platform', choices=TARGETS)
    args = parser.parse_args()
    private_key = os.environ.pop('UPDATE_LOCAL_KEY').strip().encode('ascii') + b'\n'
    sign_update(args.release_dir, args.version_file, args.keys, args.platform, private_key)


if __name__ == '__main__':
    main()
