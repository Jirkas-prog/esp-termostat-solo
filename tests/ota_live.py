"""Bench integration test: rejected/interrupted OTA must leave the running image intact."""
import argparse
import http.client
import json
import socket
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from ota import image_identity, read_json

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary', type=Path)
parser.add_argument('--url', default='http://ter01.local')
args = parser.parse_args()
base = args.url.rstrip('/')
data = args.binary.read_bytes()
image_identity(data)
before = read_json(base+'/api/state')
info = read_json(base+'/api/info')
assert before['active_relay_mode'] == 'none', 'Run destructive-input tests only on bench in NONE mode'
assert info['ota']['supported'] and not info['ota']['pending_verify']

def prepare():
    request = urllib.request.Request(base+'/api/update/prepare', data=b'', headers={'X-Thermostat-Token': info['token']})
    with urllib.request.urlopen(request, timeout=8) as response:
        assert json.load(response)['ready']

def rejected(payload, code=400, token=None, content_type='application/octet-stream'):
    if token is None:
        prepare()
    request = urllib.request.Request(base+'/api/update', data=payload, headers={
        'Content-Type': content_type, 'X-Thermostat-Token': info['token'] if token is None else token})
    try:
        with urllib.request.urlopen(request, timeout=90) as response:
            raise AssertionError(f'Invalid update accepted: {response.status}')
    except urllib.error.HTTPError as error:
        message = error.read(); error.close()
        assert error.code == code, (error.code, message)
    except (ConnectionResetError, http.client.RemoteDisconnected):
        # The server closes rejected requests without draining their binary body.
        # TCP may discard the error response when unread payload is still queued.
        pass
    after = read_json(base+'/api/state')
    assert after['boot'] == before['boot'] and after['settings'] == before['settings']
    print(f'PASS: rejected {len(payload)} bytes (HTTP {code} or closed connection); original boot retained', flush=True)

rejected(data[:288], 403, token='wrong')
rejected(data[:288], 415, content_type='application/json')
rejected(b'not a firmware')
wrong_project = bytearray(data[:4096]); wrong_project[80] ^= 1
rejected(wrong_project)
old_version = bytearray(data[:4096]); old_version[48:80] = b'0.4.2'+bytes(27)
rejected(old_version)
corrupt = bytearray(data); corrupt[10000] ^= 1
rejected(corrupt)
print('PASS: unauthorized, wrong type/size/project, pre-OTA version and corrupt image rejected', flush=True)

address = urllib.parse.urlsplit(base)
prepare()
headers = (f'POST /api/update HTTP/1.1\r\nHost: {address.netloc}\r\n'
           f'X-Thermostat-Token: {info["token"]}\r\nContent-Type: application/octet-stream\r\n'
           f'Content-Length: {len(data)}\r\nConnection: close\r\n\r\n').encode()
with socket.create_connection((address.hostname, address.port or 80), timeout=5) as connection:
    connection.sendall(headers+data[:8192])
    connection.shutdown(socket.SHUT_WR)
    try:
        connection.recv(1024)
    except (TimeoutError, ConnectionError):
        pass
for _ in range(8):
    time.sleep(1)
    after = read_json(base+'/api/state')
    if after['sensor1']['valid'] and after['sensor2']['valid']:
        break
assert after['boot'] == before['boot'] and after['settings'] == before['settings']
assert read_json(base+'/api/info')['ota']['slot'] == info['ota']['slot']
assert after['sensor1']['valid'] and after['sensor2']['valid']
print('PASS: interrupted upload leaves original boot/slot/settings intact; both sensors continue measuring')
