"""Verify compiled release, source/staging identity, HA templates and local docs.
Requires PyYAML and Jinja2. --stage can point to an alternative build workspace.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import xml.etree.ElementTree as ET
root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/'test-output/python'))
import yaml
from jinja2 import Environment, StrictUndefined

args=argparse.ArgumentParser()
args.add_argument('--stage',type=Path,default=Path('C:/Espressif/thermostat-40-solo'))
stage=args.parse_args().stage
release=root/'releases/0.8.5'
manifest=json.loads((release/'manifest.json').read_text())
assert f"const PAGE_FIRMWARE='solo-{manifest['version']}';" in (root/'firmware/main/web/index.html').read_text(encoding='utf-8'), 'Web/firmware version mismatch'
def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()
assert manifest['project']=='ESP TERMOSTAT SOLO' and manifest['output_default']=='none'
for name,expected in manifest['sha256'].items(): assert digest(release/name)==expected,name
for name,expected in manifest['source_sha256'].items():
    assert digest(root/name)==expected,('changed source',name)
    assert digest(stage/name)==expected,('stale build',name)
app=(release/'esp_termostat_solo.bin').read_bytes()
factory=(release/'factory.bin').read_bytes()
assert app[0]==0xe9 and struct.unpack_from('<H',app,12)[0]==0,'Wrong target chip'
for asset in ['index.html','logo.svg']:
    assert (root/'firmware/main/web'/asset).read_bytes() in app,('web asset missing from image',asset)
assert 'CONFIG_HTTPD_MAX_REQ_HDR_LEN=1536' in (stage/'firmware/sdkconfig').read_text()
assert factory[0x10000:0x10000+len(app)]==app
for name,address in [('bootloader.bin',0x1000),('partition-table.bin',0x8000)]:
    data=(release/name).read_bytes(); assert factory[address:address+len(data)]==data
assert factory[0x9000:0xf000]==b'\xff'*0x6000,'Factory must clearly reset NVS'
parts=(release/'partition-table.bin').read_bytes(); entries=[]
for i in range(0,len(parts),32):
    entry=parts[i:i+32]
    if entry[:2]!=b'\xaaP': break
    _,typ,sub,address,size,name,flags=struct.unpack('<HBBII16sI',entry)
    assert address+size<=4*1024*1024
    entries.append((typ,sub,address,size))
assert [(a,n) for t,s,a,n in entries if t==0 and s in (0x10,0x11)]==[(0x10000,0x1e0000),(0x1f0000,0x1e0000)]
assert all(len(app)<=n for t,s,a,n in entries if t==0)
assert (1,0,0x3d0000,0x2000) in entries
assert all(a+n<=b for (_,_,a,n),(_,_,b,_) in zip(entries,entries[1:]))
assert factory[0x3d0000:0x3d2000]==b'\xff'*0x2000
assert 'CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y' in (stage/'firmware/sdkconfig').read_text()
print(f'PASS ESP32 image: {len(app):,} B app, {len(factory):,} B factory; partitions, hashes, source and staged build match')

class UniqueLoader(yaml.SafeLoader): pass
def mapping(loader,node,deep=False):
    out={}
    for k,v in node.value:
        key=loader.construct_object(k,deep=deep)
        assert key not in out,('duplicate YAML key',key)
        out[key]=loader.construct_object(v,deep=deep)
    return out
UniqueLoader.add_constructor(yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG,mapping)
config=yaml.load((root/'home-assistant/termostat_solo.yaml').read_text(encoding='utf-8'),Loader=UniqueLoader)
assert len(config['mqtt']['sensor'])==7 and len(config['mqtt']['binary_sensor'])==6
ids=[e['unique_id'] for values in config['mqtt'].values() for e in values]
assert len(ids)==len(set(ids)) and all(x.startswith('kamna_solo_') for x in ids)
ack=config['automation'][0]['actions'][0]['data']
assert ack['topic']=='wifi2mqtt/termostat-solo/ack' and ack['retain'] is False
env=Environment(undefined=StrictUndefined)
def render(template,data):return env.from_string(template).render(**data)
payload={'inlet':40.0,'outlet':60.0,'average':50.0,'minimum':40.0,'maximum':60.0,'difference':20.0,
    'sequence':17,'boot':123,'faults':0,'sensor1':{'valid':True},'sensor2':{'valid':True},'pump_request':True,'output_enabled':False}
for sensor in config['mqtt']['sensor'][:6]:
    assert render(sensor['availability_template'],{'value_json':payload})=='online'
    float(render(sensor['value_template'],{'value_json':payload}))
    key=re.search(r'value_json\.(\w+)',sensor['value_template']).group(1)
    missing=dict(payload); missing[key]=None
    assert render(sensor['availability_template'],{'value_json':missing})=='offline'
assert 'device_class' not in config['mqtt']['sensor'][5] # delta must not get absolute-temperature conversion
for entity in config['mqtt']['binary_sensor']:
    assert render(entity['value_template'],{'value_json':payload}) in ('ON','OFF')
assert render(config['mqtt']['binary_sensor'][1]['value_template'],{'value_json':payload})=='OFF'
assert render(ack['payload'],{'trigger':{'payload_json':payload}})=='17'
assert render(config['automation'][1]['conditions'][0]['value_template'],{'trigger':{'from_state':{'state':'0'},'to_state':{'state':'1'}}})=='True'
print('PASS HA YAML, unique IDs, valid/null measurement templates, disabled output, delta units and non-retained ACK')

ET.parse(root/'docs/ZAPOJENI.svg')
for p in [root/'README.md',*list((root/'docs').glob('*.md')),root/'home-assistant/README.md']:
    for dest in re.findall(r'\]\(([^)]+)\)',p.read_text(encoding='utf-8')):
        if '://' not in dest and not dest.startswith('#'):
            assert (p.parent/dest.split('#')[0]).exists(),(p,dest)
print('PASS SVG XML and local documentation links')
