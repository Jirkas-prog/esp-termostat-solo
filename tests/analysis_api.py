"""Live rejection tests for the model API; valid saved settings must remain unchanged."""
import argparse
import copy
import json
import urllib.error
import urllib.request

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url',default='http://ter01.local')
    url=parser.parse_args().url.rstrip('/')
    def get(path):
        with urllib.request.urlopen(url+path,timeout=5) as response:return json.load(response)
    before=get('/api/analysis'); info=get('/api/info')
    assert before['saved'] and before['model']=='thermal-v1'
    base={'schema':1,'revision':before['revision'],'parameters':before['parameters']}
    cases=[]
    for key,value in [('ambient_c',51),('ambient_c','25'),('enabled',0.5),('water_kj_l_k',None)]:
        data=copy.deepcopy(base);data['parameters'][key]=value;cases.append((key,json.dumps(data),400))
    data=copy.deepcopy(base);data['parameters'].pop('ambient_c');cases.append(('missing',json.dumps(data),400))
    data=copy.deepcopy(base);data['parameters']['foreign']=0;cases.append(('unknown',json.dumps(data),400))
    data=copy.deepcopy(base);data['parameters']['max_gap_s']=15;data['parameters']['trend_s']=15;cases.append(('gap/window',json.dumps(data),400))
    data=copy.deepcopy(base);data['parameters']['heat_model_enabled']=1;data['parameters']['reference_rate']=0;cases.append(('uncalibrated heat',json.dumps(data),400))
    data=copy.deepcopy(base);data['revision']=0;cases.append(('stale revision',json.dumps(data),409))
    raw=json.dumps(base).replace('"parameters": {','"parameters": {"ambient_c": 25,',1)
    cases.append(('duplicate key',raw,400))
    for label,raw,expected in cases:
        request=urllib.request.Request(url+'/api/analysis',data=raw.encode(),headers={'Content-Type':'application/json','X-Thermostat-Token':info['token']})
        try:
            with urllib.request.urlopen(request,timeout=5) as response:code=response.status
        except urllib.error.HTTPError as error:code=error.code
        assert code==expected,(label,code)
        after=get('/api/analysis')
        assert after['revision']==before['revision'] and after['parameters']==before['parameters'],label
    print('PASS: model API rejects invalid values, missing/unknown/duplicate fields, inconsistent model and stale revision; saved parameters unchanged')

if __name__=='__main__':main()
