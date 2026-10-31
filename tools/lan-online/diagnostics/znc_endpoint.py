#!/usr/bin/env python3
"""Private TLS request-metadata diagnostic; deliberately implements no ZNC API."""

import argparse
import ipaddress
import json
from http.server import BaseHTTPRequestHandler, HTTPServer
import re
import ssl
import sys

MAX_BODY = 64 * 1024
SOCKET_TIMEOUT = 5
KNOWN_PATHS = frozenset('/v1/NX/' + name for name in (
    'getToken', 'activateRoom', 'deactivateRoom', 'ping',
    'setVoiceChatChannel', 'setRoomStatus'))
CONTENT_TYPES = frozenset(('application/json', 'application/octet-stream',
                           'application/x-www-form-urlencoded', 'text/plain'))
PRIVATE_NETWORKS = tuple(ipaddress.IPv4Network(value) for value in (
    '10.0.0.0/8', '172.16.0.0/12', '192.168.0.0/16'))


def private_ipv4(value):
    try:
        address = ipaddress.IPv4Address(value)
    except ipaddress.AddressValueError:
        raise argparse.ArgumentTypeError('bind must be a private IPv4 literal') from None
    if not any(address in network for network in PRIVATE_NETWORKS):
        raise argparse.ArgumentTypeError('bind must be a private IPv4 literal')
    return str(address)


class DiagnosticServer(HTTPServer):
    """Single connection at a time, with bounded reads and TLS handshakes."""

    def __init__(self, address, audit, tls_context=None):
        self.audit = audit
        self.tls_context = tls_context
        super().__init__(address, DiagnosticHandler)

    def get_request(self):
        connection, address = super().get_request()
        connection.settimeout(SOCKET_TIMEOUT)
        if self.tls_context is not None:
            try:
                connection = self.tls_context.wrap_socket(connection, server_side=True)
            except (OSError, ValueError):
                connection.close()
                raise OSError('TLS connection rejected') from None
        return connection, address

    def handle_error(self, request, client_address):
        # BaseServer prints tracebacks; they can contain request-controlled data.
        pass


class DiagnosticHandler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.0'
    server_version = 'PrivateDiagnostic'
    sys_version = ''

    def log_message(self, format, *args):
        pass

    def send_error(self, code, message=None, explain=None):
        self.respond(code, b'Unsupported diagnostic request.\n')

    def respond(self, code, body):
        self.close_connection = True
        self.send_response(code)
        self.send_header('Content-Type', 'text/plain; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Connection', 'close')
        self.end_headers()
        if self.command != 'HEAD':
            self.wfile.write(body)

    def handle_diagnostic(self):
        # Only known constant paths/types are emitted. Unknown path segments and
        # MIME strings may themselves contain credentials, so redact them whole.
        path = self.path.split('?', 1)[0]
        content_type = self.headers.get('Content-Type', '').split(';', 1)[0].strip().lower()
        metadata = {
            'method': self.command if re.fullmatch('[A-Z]{1,16}', self.command) else '<other>',
            'path': path if path in KNOWN_PATHS else '<unknown>',
            'content_type': content_type if content_type in CONTENT_TYPES else '<other>',
            'body_length': 0,
        }
        status = 503
        lengths = self.headers.get_all('Content-Length', [])
        length = 0
        if self.headers.get_all('Transfer-Encoding'):
            status = 400
        elif lengths:
            value = lengths[0]
            if len(lengths) != 1 or not re.fullmatch('[0-9]{1,10}', value):
                status = 400
            else:
                length = int(value)
                if length > MAX_BODY:
                    status = 413
        if status == 503:
            remaining = length
            try:
                while remaining:
                    chunk = self.rfile.read(min(4096, remaining))
                    if not chunk:
                        status = 400
                        break
                    remaining -= len(chunk)
                    metadata['body_length'] += len(chunk)
            except OSError:
                status = 400
        self.server.audit(metadata)
        self.respond(status, b'Unsupported ZNC diagnostic endpoint.\n')

    do_GET = do_POST = do_PUT = do_PATCH = do_DELETE = do_OPTIONS = do_HEAD = handle_diagnostic


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bind', type=private_ipv4, required=True)
    parser.add_argument('--port', type=int, default=8444, choices=range(1, 65536), metavar='PORT')
    parser.add_argument('--cert', required=True)
    parser.add_argument('--key', required=True)
    args = parser.parse_args(argv)
    try:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(args.cert, args.key)
        with DiagnosticServer((args.bind, args.port),
                              lambda row: print(json.dumps(row), flush=True), context) as server:
            server.serve_forever()
    except (OSError, ValueError):
        print('ZNC diagnostic startup or listener failure.', file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
