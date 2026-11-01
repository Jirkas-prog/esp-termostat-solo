"""Upload a SOLO application over LAN, then verify reboot, image and retained settings."""
import argparse
import hashlib
import getpass
import copy
import http.client
import json
import struct
import time
import urllib.error
import urllib.request
import urllib.parse
from pathlib import Path


def image_identity(data):
    if len(data) < 288 or data[0] != 0xe9 or struct.unpack_from('<H', data, 12)[0] != 0 or data[23] != 1:
        raise ValueError('Expected ESP32 application BIN with SHA-256; not factory.bin')
    if struct.unpack_from('<I', data, 32)[0] != 0xabcd5432 or data[80:112].split(b'\0')[0] != b'esp_termostat_solo':
        raise ValueError('Not an ESP TERMOSTAT SOLO application')
    version = data[48:80].split(b'\0')[0].decode('ascii')
    parts = tuple(int(p) for p in version.split('.'))
    if len(parts) != 3 or parts < (0, 5, 3):
        raise ValueError('OTA requires SOLO 0.5.3 or newer')
    if hashlib.sha256(data[:-32]).digest() != data[-32:]:
        raise ValueError('Application SHA-256 mismatch')
    return version, data[176:208].hex()


def read_json(url, session=""):
    request=urllib.request.Request(url,headers={"X-Thermostat-Session":session})
    with urllib.request.urlopen(request, timeout=5) as response:
        return json.load(response)


def authenticate(url,info,session_file=None):
    session=Path(session_file).read_text(encoding='utf-8').strip() if session_file and Path(session_file).exists() else ''
    if session and read_json(url+'/api/auth',session).get('authenticated'):
        return session
    def post(path,body):
        request=urllib.request.Request(url+path,data=json.dumps(body).encode(),headers={'Content-Type':'application/json','X-Thermostat-Token':info['token']})
        with urllib.request.urlopen(request,timeout=6) as response:return json.load(response)
    try:post('/api/auth/display',{})
    except urllib.error.HTTPError as error:
        if error.code!=429:raise
    print('Settings are protected. Read the four-digit PIN on the device display.',flush=True)
    pin=getpass.getpass('Device PIN: ')
    if len(pin)!=4 or not pin.isascii() or not pin.isdigit():raise ValueError('Expected four digits')
    result=post('/api/auth/login',{'pin':pin});session=result['session']
    if session_file:
        path=Path(session_file);path.parent.mkdir(parents=True,exist_ok=True);path.write_text(session,encoding='utf-8')
    return session


def migrated_network(config):
    expected=copy.deepcopy(config)
    if expected['schema']==1:
        expected['schema']=2
        expected['order']=[i for i,w in sorted(enumerate(config['wifi']),key=lambda item:-item[1]['priority']) if w['ssid']]+['ap']
        expected['wifi']=[dict(id=i,**{k:v for k,v in w.items() if k!='priority'}) for i,w in enumerate(config['wifi']) if w['ssid']]
        if expected['mqtt']['publish_s']==5:expected['mqtt']['publish_s']=60
        expected['mqtt'].update(min_publish_s=1,delta_centi=20,publish_pin=True)
    return expected


def deploy(url, binary, session_file=None):
    data = Path(binary).read_bytes()
    version, identity = image_identity(data)
    url = url.rstrip('/')
    address = urllib.parse.urlsplit(url)
    if address.scheme != 'http' or address.path or address.query or address.fragment:
        raise ValueError('Use the device base URL, e.g. http://ter01.local')
    connection = http.client.HTTPConnection(address.hostname, address.port or 80, timeout=150)
    session=""
    def get(path):
        connection.request('GET', path,headers={"X-Thermostat-Session":session})
        response = connection.getresponse()
        result = json.loads(response.read())
        if response.status != 200:
            raise RuntimeError(f'Device returned HTTP {response.status}')
        return result
    try:
        before = get('/api/state')
        info = get('/api/info')
        if info.get('auth_required'):session=authenticate(url,info,session_file)
        model_before = get('/api/analysis') if 'analysis' in before else None
        network_before = get('/api/network') if tuple(int(v) for v in info.get('ota', {}).get('version', '0.0.0').split('.')) >= (0, 7, 0) else None
    except Exception:
        connection.close()
        raise
    ota = info.get('ota', {})
    if not ota.get('supported') or ota.get('pending_verify') or len(data) > ota.get('max_size', 0):
        connection.close()
        raise RuntimeError('Device OTA unavailable, awaiting boot validation, or image too large')
    print(f'Uploading SOLO {version}, {len(data)} bytes over OTA...', flush=True)
    try:
        if ota.get('prepare_required'):
            connection.request('POST', '/api/update/prepare', body=b'', headers={'X-Thermostat-Token': info['token'],'X-Thermostat-Session':session})
            response = connection.getresponse()
            result = json.loads(response.read())
            if response.status != 200 or not result.get('ready'):
                raise RuntimeError(result.get('error', 'Sensors could not be paused'))
            print('Device ready; sensor peripherals paused, probes remain connected.', flush=True)
        def blocks():
            # Pace flash-sized writes on the ESP32 LAN link; keep explicit length
            # so an iterable body does not switch to unsupported chunked encoding.
            for offset in range(0, len(data), 4096):
                yield data[offset:offset+4096]
                time.sleep(0.02)
        connection.request('POST', '/api/update', body=blocks(), headers={
            'Content-Length': str(len(data)),
            'Content-Type': 'application/octet-stream', 'X-Thermostat-Token': info['token'],'X-Thermostat-Session':session})
        response = connection.getresponse()
        result = json.loads(response.read())
        if response.status != 200:
            raise RuntimeError(result.get('error', f'HTTP {response.status}'))
        assert result.get('restarting'), 'Update was not accepted'
    except (TimeoutError, ConnectionError, http.client.RemoteDisconnected):
        print('Connection interrupted; checking whether update completed (no automatic re-upload).', flush=True)
    finally:
        connection.close()
    deadline = time.monotonic()+80
    while time.monotonic() < deadline:
        time.sleep(2)
        try:
            after = read_json(url+'/api/state')
            actual_info=read_json(url+'/api/info')
            actual=actual_info['ota']
        except (urllib.error.URLError, TimeoutError, ConnectionError):
            continue
        if after['boot'] == before['boot']:
            continue
        if actual['elf_sha256'] != identity or actual['slot'] == ota['slot']:
            raise RuntimeError('Device restarted into a different image or rolled back')
        if actual['pending_verify']:
            continue
        assert after['settings'] == before['settings'], 'Settings changed during update'
        for sensor in ['sensor1', 'sensor2']:
            assert after[sensor]['calibration'] == before[sensor]['calibration'], 'Calibration changed'
        assert after['firmware'] == 'solo-'+version
        matching_integrity=info.get('integrity') and actual_info.get('integrity')==info['integrity']
        if not matching_integrity:
            after_session=authenticate(url,actual_info,session_file) if actual_info.get('auth_required') else ''
            if model_before is not None:
                model_after=read_json(url+'/api/analysis',after_session)
                assert model_after['saved'], 'Analysis parameters not persisted'
                for key in ['schema','model','revision','parameters']:
                    assert model_after[key]==model_before[key], f'Analysis {key} changed during OTA'
            if network_before is not None:
                network_after=read_json(url+'/api/network',after_session)
                expected=migrated_network(network_before) if network_after['schema']==2 else network_before
                assert network_after==expected, 'Unexpected network change during OTA/migration'
        print(f"PASS: {after['firmware']} confirmed in {actual['slot']}; image, reboot, settings, calibrations and existing model/network parameters verified.")
        return after
    raise RuntimeError('OTA boot confirmation not received; check device without retrying flash blindly')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--url', default='http://ter01.local')
    parser.add_argument('--session-file',type=Path,help='Private file for the short-lived login session; PIN is never saved')
    args = parser.parse_args()
    deploy(args.url, args.binary,args.session_file)
