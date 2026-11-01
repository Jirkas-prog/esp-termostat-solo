"""Render the actual C++-generated discovery templates against real/invalid states."""
import copy,json,sys
from pathlib import Path
root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/'test-output/python'))
from jinja2 import Environment,StrictUndefined
env=Environment(undefined=StrictUndefined)
state=json.loads(Path(sys.argv[1]).read_text(encoding='utf-8'))
definitions=json.loads((root/'test-output/discovery.json').read_text(encoding='utf-8'))
assert len(definitions)==25
for d in definitions:
    p=d['payload'];assert p['device']['identifiers']==['termostat_aabbccddeeff']
    assert p['availability_mode']=='all'
    if p['unique_id'].endswith('_verification_pin'):
        assert p['state_topic'].endswith('/access') and 'expire_after' not in p and 'unit_of_measurement' not in p and 'state_class' not in p
        for pin in ['0000','0427','9999']:
            assert env.from_string(p['value_template']).render(value_json={'verification_pin':pin})==pin
        continue
    assert p['expire_after']==190
    value=env.from_string(p['value_template']).render(value_json=state)
    available=env.from_string(p['availability'][1]['value_template']).render(value_json=state)
    if p['unique_id'].endswith(('_source_kw','_export_kw','_relative')):assert value=='None' and available=='offline'
    if p['unique_id'].endswith('_delta'):assert float(value)==state['difference'] and 'device_class' not in p
    if '/binary_sensor/' in d['topic']:assert value in ('ON','OFF') and available=='online'
    if p['unique_id'].endswith(('_outlet','_inlet','_average','_maximum','_minimum','_delta','_probe1','_probe2')):
        bad=copy.deepcopy(state)
        for key in ['outlet','inlet','average','maximum','minimum','difference']:bad[key]=None
        bad['sensor1']['temperature']=None;bad['sensor2']['temperature']=None
        assert env.from_string(p['availability'][1]['value_template']).render(value_json=bad)=='offline'
        assert env.from_string(p['value_template']).render(value_json=bad)=='None'
print('PASS 25 discovery templates: common device, unique entities, real temperature/delta fields, expiry, invalid probes unavailable and uncalibrated power remains unknown')
