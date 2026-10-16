#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run standalone fixture probes in fresh processes without network or account access."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config-probe', type=Path, required=True)
    parser.add_argument('--token-probe', type=Path, required=True)
    args = parser.parse_args()
    config_exe = args.config_probe.resolve(strict=True)
    token_exe = args.token_probe.resolve(strict=True)
    config = dict(account_id=49153,
                  dns={'auth.fixture.test': '192.168.10.20',
                       'g23380901-lp1.s.n.srv.nintendo.net': '192.168.10.20'},
                  allowed_ips=['192.168.10.20', '192.168.10.21'],
                  port_redirects=[dict(address='192.168.10.20', **{'from': 443, 'to': 8443})])
    cases = [('disabled', None, 'disabled'), ('valid', config, 'valid')]
    for label, edit in [
        ('public-ip', {'allowed_ips': ['8.8.8.8']}),
        ('zero-identity', {'account_id': 0}),
        ('negative-identity', {'account_id': -1}),
        ('overflow-identity', {'account_id': 2**32}),
        ('unlisted-dns-ip', {'dns': {'auth.fixture.test': '192.168.10.99'}}),
        ('ambiguous-ip', {'allowed_ips': ['192.168.010.20']}),
        ('port-overflow', {'port_redirects': [dict(address='192.168.10.20',
                                                  **{'from': 443, 'to': 65536})]}),
        ('duplicate-port', {'port_redirects': config['port_redirects'] * 2}),
        ('template-key', {'dns': {'g23380901-%.s.n.srv.nintendo.net': '192.168.10.20'}}),
        ('normalized-duplicate-dns', {'dns': {'AUTH.FIXTURE.TEST.': '192.168.10.20',
                                              'auth.fixture.test': '192.168.10.20'}}),
    ]:
        cases.append((label, config | edit, 'invalid'))
    passed = 0
    with tempfile.TemporaryDirectory(prefix='eden-nso-lan-probes-') as directory:
        directory = Path(directory)
        for label, value, mode in cases + [('missing-file', None, 'invalid'),
                                         ('empty-path', None, 'invalid'),
                                         ('oversized-file', None, 'invalid')]:
            env = os.environ.copy()
            env.pop('EDEN_NSO_LAN_CONFIG', None)
            if value is not None:
                path = directory / (label + '.json')
                path.write_text(json.dumps(value), encoding='utf-8')
                env['EDEN_NSO_LAN_CONFIG'] = str(path)
            elif label == 'missing-file':
                env['EDEN_NSO_LAN_CONFIG'] = str(directory / 'absent.json')
            elif label == 'empty-path':
                env['EDEN_NSO_LAN_CONFIG'] = ''
            elif label == 'oversized-file':
                path = directory / 'oversized.json'
                path.write_text(json.dumps(config) + ' ' * 65537, encoding='utf-8')
                env['EDEN_NSO_LAN_CONFIG'] = str(path)
            # Capture only pass/failure status, not exception/environment dumps.
            process = subprocess.run([str(config_exe), mode], env=env,
                                     capture_output=True, timeout=10, check=False)
            if process.returncode:
                raise SystemExit(f'FAIL config case: {label}; exit={process.returncode}')
            passed += 1
        env = os.environ.copy()
        env.pop('EDEN_NSO_LAN_CONFIG', None)
        process = subprocess.run([str(token_exe)], env=env, capture_output=True,
                                 timeout=20, check=False)
        if process.returncode:
            raise SystemExit(f'FAIL token signature/claims probe; exit={process.returncode}')
    print(f'PASS: {passed} fresh-process config cases; token signature/claims probe passed')


if __name__ == '__main__':
    main()
