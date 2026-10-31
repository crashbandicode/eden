import argparse
import http.client
import json
import socket
import threading
import unittest

from znc_endpoint import DiagnosticServer, KNOWN_PATHS, MAX_BODY, private_ipv4


class EndpointTests(unittest.TestCase):
    def setUp(self):
        self.rows = []
        self.server = DiagnosticServer(('127.0.0.1', 0), self.rows.append)
        self.thread = threading.Thread(target=self.server.serve_forever,
                                       kwargs={'poll_interval': 0.01})
        self.thread.start()
        self.address = self.server.server_address

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(2)
        self.assertFalse(self.thread.is_alive())

    def request(self, method, path, body=None, headers=None):
        connection = http.client.HTTPConnection(*self.address, timeout=2)
        try:
            connection.request(method, path, body=body, headers=headers or {})
            response = connection.getresponse()
            return response.status, response.read(), response.getheader('Connection')
        finally:
            connection.close()

    def test_post_metadata_excludes_secrets(self):
        # Synthetic sentinels only; failed assertions never print request values.
        secrets = ('synthetic-auth-sentinel', 'synthetic-query-sentinel',
                   'synthetic-body-sentinel')
        body = json.dumps({'token': secrets[2], 'unknown': {'nested': secrets[2]}})
        status, response, close = self.request('POST', '/v1/NX/getToken?token=' + secrets[1],
            body, {'Content-Type': 'application/json; secret=' + secrets[0],
                   'Authorization': 'Bearer ' + secrets[0]})
        self.assertEqual(status, 503)
        self.assertIn(b'Unsupported ZNC', response)
        self.assertEqual(close, 'close')
        self.assertEqual(set(self.rows[0]), {'method', 'path', 'content_type', 'body_length'})
        self.assertEqual(self.rows[0]['body_length'], len(body))
        self.assertEqual(self.rows[0]['path'], '/v1/NX/getToken')
        self.assertTrue(all(value not in json.dumps(self.rows) for value in secrets))

    def test_known_routes_and_methods_still_unsupported(self):
        for path, method in zip(sorted(KNOWN_PATHS), ('GET', 'POST', 'PUT', 'PATCH', 'DELETE', 'OPTIONS')):
            with self.subTest(method=method):
                self.assertEqual(self.request(method, path)[0], 503)
                self.assertEqual(self.rows[-1]['path'], path)

    def test_unknown_path_and_content_type_redacted(self):
        self.assertEqual(self.request('POST', '/private-synthetic-value?secret=value', b'',
                                     {'Content-Type': 'secret/synthetic-value'})[0], 503)
        self.assertEqual(self.rows[0]['path'], '<unknown>')
        self.assertEqual(self.rows[0]['content_type'], '<other>')

    def test_maximum_body_consumed(self):
        self.assertEqual(self.request('POST', '/v1/NX/ping', b'x' * MAX_BODY)[0], 503)
        self.assertEqual(self.rows[0]['body_length'], MAX_BODY)

    def test_oversized_body_rejected_before_read(self):
        self.assertEqual(self.request('POST', '/v1/NX/ping', headers={
            'Content-Length': str(MAX_BODY + 1)})[0], 413)
        self.assertEqual(self.rows[0]['body_length'], 0)

    def test_chunked_body_rejected(self):
        self.assertEqual(self.request('POST', '/v1/NX/ping', b'0\r\n\r\n',
                                     {'Transfer-Encoding': 'chunked'})[0], 400)
        self.assertEqual(self.rows[0]['body_length'], 0)

    def raw_status(self, headers, body=b''):
        with socket.create_connection(self.address, timeout=2) as connection:
            connection.sendall(b'POST /v1/NX/ping HTTP/1.0\r\n' + headers + b'\r\n' + body)
            connection.shutdown(socket.SHUT_WR)
            response = connection.recv(4096)
        return int(response.split(b' ', 2)[1])

    def test_duplicate_lengths_rejected(self):
        self.assertEqual(self.raw_status(b'Content-Length: 0\r\nContent-Length: 1\r\n'), 400)

    def test_invalid_length_rejected(self):
        self.assertEqual(self.raw_status(b'Content-Length: -1\r\n'), 400)

    def test_truncated_body_rejected(self):
        self.assertEqual(self.raw_status(b'Content-Length: 5\r\n', b'ab'), 400)
        self.assertEqual(self.rows[0]['body_length'], 2)

    def test_head_has_no_body(self):
        status, body, _ = self.request('HEAD', '/v1/NX/ping')
        self.assertEqual((status, body), (503, b''))

    def test_bind_is_private_ipv4_literal_only(self):
        for address in ('192.168.0.240', '10.1.2.3', '172.16.0.2'):
            self.assertEqual(private_ipv4(address), address)
        for address in ('0.0.0.0', '8.8.8.8', '127.0.0.1', '::1', 'localhost', '169.254.1.1'):
            with self.assertRaises(argparse.ArgumentTypeError):
                private_ipv4(address)


if __name__ == '__main__':
    unittest.main()
