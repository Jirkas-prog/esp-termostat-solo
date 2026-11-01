"""Read-only/negative live API checks; never intentionally change stored settings."""
import copy,json,urllib.request,urllib.error,argparse,http.client
from pathlib import Path

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--url',default='http://ter01.local');p.add_argument('--session-file',type=Path,required=True);args=p.parse_args();url=args.url.rstrip('/');session=args.session_file.read_text().strip()
    def call(path,data=None,token=None,host=None,closed_ok=False):
        headers={"X-Thermostat-Session":session}
        if token:headers['X-Thermostat-Token']=token
        if host:headers['Host']=host
        if data is not None:
            headers['Content-Type']='application/json'
            data=data.encode() if isinstance(data,str) else json.dumps(data).encode()
        try:
            with urllib.request.urlopen(urllib.request.Request(url+path,data=data,headers=headers),timeout=6) as r:return r.status,json.load(r)
        except urllib.error.HTTPError as e:return e.code,json.load(e)
        except (ConnectionResetError,http.client.RemoteDisconnected):
            if closed_ok:return None,{}
            raise
    _,info=call('/api/info');_,state=call('/api/state');_,cfg=call('/api/network');token=info['token']
    assert cfg['saved'] and cfg['schema']==2 and 'ap' in cfg['order']
    assert all('password' not in w for w in cfg['wifi']) and 'password' not in cfg['ap'] and 'password' not in cfg['mqtt']
    body={k:copy.deepcopy(cfg[k]) for k in ['schema','revision','wifi','order','ap','mqtt']}
    for group in [*body['wifi'],body['ap'],body['mqtt']]:group.pop('password_set',None)
    assert call('/api/network',body,closed_ok=True)[0] in (403,None)
    assert call('/api/network',host='not-this-device.invalid')[0]==403
    bad=copy.deepcopy(body);bad['revision']+=1;assert call('/api/network',bad,token)[0]==409
    for group,key,value in [('mqtt','host','mqtt://bad-host'),('mqtt','topic','bad/#'),('mqtt','port',0),('ap','password',''),('ap','delay_s',0)]:
        bad=copy.deepcopy(body);bad[group][key]=value;assert call('/api/network',bad,token)[0]==400,(group,key)
    bad=copy.deepcopy(body);bad['order'].remove('ap');assert call('/api/network',bad,token)[0]==400
    bad=copy.deepcopy(body);bad['wifi'][0]['ssid']='different-ssid-without-new-key';assert call('/api/network',bad,token)[0]==400
    bad=json.dumps(body)[:-1]+',"revision":'+str(body['revision'])+'}';assert call('/api/network',bad,token)[0]==400
    _,after=call('/api/network');_,current=call('/api/state');assert after==cfg and current['boot']==state['boot']
    assert current['settings']==state['settings']
    print('PASS live network API: redaction, token/Host protection, validation, duplicate rejection and revision conflicts; configuration and boot unchanged')
if __name__=='__main__':main()
