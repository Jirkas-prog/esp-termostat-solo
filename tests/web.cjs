// Run with Node + Playwright installed; on this workstation use the bundled runtime and NODE_PATH.

const {chromium}=require('playwright');

const http=require('http'),fs=require('fs'),path=require('path'),assert=require('assert/strict');

const root=path.resolve(__dirname,'..'),out=path.join(root,'test-output','web');fs.mkdirSync(out,{recursive:true});

const reference=process.argv[2],otaBinary=fs.readFileSync(path.join(root,'releases/0.8.5/esp_termostat_solo.bin'));

let stalePageOnce=false,htmlRequests=0;
const server=http.createServer((req,res)=>{

  const file=req.url==='/logo.svg'?'logo.svg':'index.html';

  if(req.url==='/reference'&&reference){res.setHeader('Content-Type','text/html; charset=utf-8');return res.end(fs.readFileSync(reference))}

  res.setHeader('Content-Type',file.endsWith('svg')?'image/svg+xml':'text/html; charset=utf-8');

  let content=fs.readFileSync(path.join(root,'firmware/main/web',file));
  if(req.url==='/'){htmlRequests++;if(stalePageOnce){stalePageOnce=false;content=content.toString('utf8').replace("const PAGE_FIRMWARE='solo-0.8.5';","const PAGE_FIRMWARE='solo-0.8.4';")}}
  res.end(content);

});

const baseState={firmware:'solo-0.8.5',sequence:1,boot:12,uptime_s:480,saved:true,link:true,mqtt_connected:true,ap_active:false,ha_delivery:false,ip:'192.0.2.58',inlet:54.3,outlet:65.4,average:59.85,minimum:54.3,maximum:65.4,difference:11.1,pump_request:true,output_enabled:false,active_relay_mode:'none',command_sent:null,manual_remaining_s:0,faults:64,sensor1:{valid:true,raw:54.3,temperature:54.3,role:'inlet',calibration:0},sensor2:{valid:true,raw:65.4,temperature:65.4,role:'outlet',calibration:0},display:{temperature:65.4,night:true,diagnostics:false,clock_valid:true,local_minute:1323,brightness:10,ready:true},settings:{revision:5,outlet_sensor:2,display_rotation:0,on_source:'maximum',off_source:'maximum',on:60,off:55,force:85,min_on_s:60,manual_s:600,relay_mode:'none',pulse_ms:50,commissioned:false,display_source:'maximum',display_off:30,display_temp_off_enabled:true,display_night_enabled:true,display_night_start:1320,display_night_end:420,display_day_brightness:100,display_night_brightness:10}};

const analysisFields=fs.readFileSync(path.join(root,'common/analysis_fields.inc'),'utf8').split(/\r?\n/).map(line=>line.match(/^X\((\w+),([^,]+),([^,]+),([^,]+),([^,]+),"([^"]+)","([^"]+)","([^"]+)"\)$/)).filter(Boolean).map(m=>({key:m[1],default:Number(m[2]),min:Number(m[3]),max:Number(m[4]),step:Number(m[5]),label:m[6],group:m[7],unit:m[8]}));

baseState.analysis={stove_state:'heating',quality:'ready',thermal_state:'warming',circulation:'unknown',model_status:'not_calibrated',config_revision:1,relative_output_pct:null,power_trend:'unknown',inlet_rate_c_min:0.1,outlet_rate_c_min:0.2,advice:'none'};

(async()=>{await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));const origin=`http://127.0.0.1:${server.address().port}`;

 const browser=await chromium.launch({channel:process.env.BROWSER_CHANNEL||'msedge',headless:true});

 try{const page=await browser.newPage({viewport:{width:1440,height:1100}});const errors=[];page.on('pageerror',e=>errors.push(e.message));

 if(reference){await page.goto(origin+'/reference');await page.screenshot({path:path.join(out,'reference.png')})}

 let s=structuredClone(baseState),offline=false,hostname='ter01';let networkConfig=JSON.parse(fs.readFileSync(path.join(root,'test-output/network-fixture.json'),'utf8'));let otaPrepared=false;const otaInfo={supported:true,pending_verify:false,version:'0.8.5',slot:'ota_0',max_size:1966080,elf_sha256:'a'.repeat(64)};const writes=[];let analysisConfig={schema:1,model:'thermal-v1',revision:1,saved:true,fields:analysisFields,parameters:Object.fromEntries(analysisFields.map(f=>[f.key,f.default]))};

 let sessionActive=false,sessionValue='',generation=0,displayRequested=false,protectedReads=0,activities=0;const authenticated=req=>sessionActive&&req.headers()['x-thermostat-session']===sessionValue;

 await page.route('**/api/**',async route=>{const req=route.request(),endpoint=new URL(req.url()).pathname;

  if(offline)return route.abort();

  if(endpoint==='/api/info')return route.fulfill({json:{hostname,topic:'wifi2mqtt/termostat-solo',token:'test-token',free_heap:120000,auth_required:true,integrity:{network:JSON.stringify(networkConfig),analysis:JSON.stringify(analysisConfig.parameters)},ota:otaInfo}});

  if(endpoint==='/api/auth')return route.fulfill({json:{ready:true,authenticated:authenticated(req),expires_in_s:authenticated(req)?300:0,retry_after_s:0,display_retry_s:0}});

  if(endpoint.startsWith('/api/auth/')){assert.equal(req.headers()['x-thermostat-token'],'test-token');if(endpoint==='/api/auth/login'){if(req.postDataJSON().pin!=='0427')return route.fulfill({status:401,json:{error:'Nesprávný PIN',retry_after_s:1}});sessionActive=true;sessionValue=String(++generation).padStart(32,'a');return route.fulfill({json:{session:sessionValue,authenticated:true,expires_in_s:300}})}if(endpoint==='/api/auth/display'){displayRequested=true;return route.fulfill({json:{authenticated:authenticated(req),display_retry_s:60}})}if(endpoint==='/api/auth/logout')sessionActive=false;else{activities++;assert(authenticated(req))}return route.fulfill({json:{authenticated:authenticated(req),expires_in_s:300}})}

  if((endpoint==='/api/network'||endpoint==='/api/analysis'||req.method()==='POST')&&!authenticated(req))return route.fulfill({status:401,json:{error:'Odemkněte PINem'}});

  if(endpoint==='/api/network'&&req.method()==='GET'){protectedReads++;return route.fulfill({json:networkConfig})}

  if(endpoint==='/api/state')return route.fulfill({json:s});

  if(endpoint==='/api/analysis'&&req.method()==='GET')return route.fulfill({json:analysisConfig});

  assert.equal(req.headers()['x-thermostat-token'],'test-token');

  if(endpoint==='/api/update/prepare'){assert.equal(req.postData(),null);otaPrepared=true;return route.fulfill({json:{ready:true}})}

  if(endpoint==='/api/update'){assert(otaPrepared);const image=req.postDataBuffer();assert.equal(image[0],0xe9);assert.equal(req.headers()['content-type'],'application/octet-stream');otaInfo.elf_sha256=image.subarray(176,208).toString('hex');otaInfo.slot='ota_1';s.boot++;sessionActive=false;return route.fulfill({json:{ok:true,restarting:true}})}

  const body=req.postDataJSON();writes.push({endpoint,body});

  if(endpoint==='/api/analysis'){if(body.revision!==analysisConfig.revision)return route.fulfill({status:409,json:{error:'Model se zmenil'}});analysisConfig.parameters=body.parameters;analysisConfig.revision++;s.analysis.config_revision=analysisConfig.revision;return route.fulfill({json:analysisConfig})}

  if(endpoint==='/api/settings'){

   if(body.revision!==s.settings.revision)return route.fulfill({status:409,json:{error:'Nastavení se změnilo. Načtěte aktuální hodnoty.'}});

   for(const[k,v]of Object.entries(body.settings))if(k.startsWith('calibration'))s[k.includes('1')?'sensor1':'sensor2'].calibration=v/100;else s.settings[k.replace(/_centi$/,'')]=k.endsWith('_centi')?v/100:v;

   s.settings.revision++;for(const i of [1,2]){s['sensor'+i].temperature=s['sensor'+i].raw+s['sensor'+i].calibration;s['sensor'+i].role=i===s.settings.outlet_sensor?'outlet':'inlet'}s.outlet=s['sensor'+s.settings.outlet_sensor].temperature;s.inlet=s['sensor'+(3-s.settings.outlet_sensor)].temperature;s.difference=s.outlet-s.inlet;s.display.temperature=s.outlet;s.display.threshold_temperature=s[s.settings.display_source];return route.fulfill({json:s});

  }

  if(endpoint==='/api/hotspot-key')return route.fulfill({json:{password:'test-only-ap-password'}});

  if(endpoint==='/api/network'){

   if(body.revision!==networkConfig.revision)return route.fulfill({status:409,json:{error:'Síť se změnila'}});

   const next=structuredClone(body);next.revision++;next.device_id=networkConfig.device_id;next.saved=true;

   for(const w of next.wifi){w.password_set=Object.hasOwn(w,'password')?!!w.password:networkConfig.wifi.find(old=>old.id===w.id)?.password_set;delete w.password}next.wifi.sort((a,b)=>a.id-b.id);

   next.ap.password_set=true;delete next.ap.password;next.mqtt.password_set=Object.hasOwn(body.mqtt,'password')?!!body.mqtt.password:networkConfig.mqtt.password_set;delete next.mqtt.password;networkConfig=next;s.boot++;sessionActive=false;

   return route.fulfill({json:{ok:true,restarting:true}});

  }

  if(endpoint==='/api/action'){s.manual_remaining_s=600;return route.fulfill({json:{ok:true}})}

  if(endpoint==='/api/hostname'){hostname=body.hostname;s.boot++;return route.fulfill({json:{ok:true,hostname,restarting:true}})}

  return route.fulfill({status:404,json:{error:'unknown'}});

 });

 await page.goto(origin);await page.getByRole('button',{name:'Upravit nastavení'}).waitFor();await page.waitForFunction(()=>document.querySelector('#connection').classList.contains('online'));

 const refresh=async()=>{const request=await page.waitForRequest(r=>r.url().endsWith('/api/state'));await request.response();await page.waitForTimeout(150)};

 const unlock=async()=>{if(await page.locator('#unlock').isVisible()){await page.locator('#pin').fill('0427');await page.locator('#pin-submit').click();await page.locator('#nastaveni').waitFor();await page.waitForTimeout(150)}};

 const hub=async()=>{if(await page.locator('#settings-open').isVisible())await page.locator('#settings-open').click();await unlock();if(await page.locator('#settings-breadcrumb').isVisible())await page.locator('#settings-breadcrumb [data-goto=nastaveni]').click()};
 const openSettings=async name=>{await hub();await page.getByRole('button',{name,exact:true}).click();await refresh()};

 assert.equal(await page.locator('header,.nav,#refresh').count(),0);assert.equal(protectedReads,0);

 await page.locator('#settings-open').click();assert(await page.locator('#unlock').isVisible());assert(await page.locator('#nastaveni').isHidden());

 await page.locator('#request-pin').click();assert(displayRequested);await page.locator('#pin').fill('1111');await page.locator('#pin-submit').click();await page.locator('#pin-form .feedback.error').waitFor();assert(await page.locator('#nastaveni').isHidden());await page.waitForTimeout(1100);

 await unlock();await refresh();assert(protectedReads>0);const passive=activities;await refresh();await refresh();assert.equal(activities,passive,'Passive polling extended session');

 for(const width of [320,375,700,1100,1440]){

  await page.setViewportSize({width,height:1050});

  for(const tab of ['Termostat','Displej','Senzory','Analýza kamen','Zařízení','Wi-Fi a hotspot','MQTT a Home Assistant','OTA aktualizace']){

   await openSettings(tab);

   assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),`Horizontal overflow at ${width}, ${tab}`);

  }

  await page.locator('#back-overview').click();

  if([375,1440].includes(width))await page.screenshot({path:path.join(out,`overview-${width}.png`),fullPage:true});

 }

 // A firmware change must preserve drafts, even after returning to the overview.
 await openSettings('Senzory');await page.locator('[name=calibration1_centi]').fill('1.75');
 const firmwareBefore=s.firmware,requestsBefore=htmlRequests;s.firmware='solo-0.8.6';await refresh();
 assert.equal(await page.locator('[name=calibration1_centi]').inputValue(),'1.75');assert.equal(htmlRequests,requestsBefore);
 await page.locator('#back-overview').click();await refresh();assert.equal(htmlRequests,requestsBefore);
 await openSettings('Senzory');assert.equal(await page.locator('[name=calibration1_centi]').inputValue(),'1.75');s.firmware=firmwareBefore;
 await page.locator('#sensors-form').getByRole('button',{name:'Zahodit změny'}).click();await page.locator('#back-overview').click();
 // Serve an older page once. Its first poll must replace it with the current UI.
 const beforeReload=htmlRequests;stalePageOnce=true;await page.goto(origin);
 await page.waitForFunction(()=>document.querySelector('#connection').classList.contains('online'));
 assert.equal(htmlRequests,beforeReload+2);assert.equal(await page.locator('#stove-status').textContent(),'Kamna hřejou.');
 console.log('PASS: stale page refreshed automatically; unsaved drafts protected on settings and overview');

 await openSettings('Analýza kamen');

 assert.equal(await page.locator('#analysis-form input').count(),analysisFields.length);assert.equal(await page.locator('#analysis-power').textContent(),'— %');
 for(const trend of ['cooling','steady','warming']){s.analysis.thermal_state=trend;await refresh();assert.equal(await page.locator('#stove-status').textContent(),'Kamna hřejou.')}

 assert.equal(await page.locator('#analysis-mixing-minimum').textContent(),'— °C');
 s.analysis.thermal_state='redistributing';s.analysis.mixing_minimum_c=40.74;s.analysis.mixing_eta_min=2.5;await refresh();
 assert.equal(await page.locator('#analysis-thermal').textContent(),'Přechod teplot při oběhu (odhad)');
 assert.equal(await page.locator('#stove-status').textContent(),'Kamna hřejou.');
 assert.match(await page.locator('#analysis-mixing-minimum').textContent(),/40[,.]7/);
 s.analysis.thermal_state='warming';s.analysis.mixing_minimum_c=null;s.analysis.mixing_eta_min=null;await refresh();
 assert.equal(await page.locator('#analysis-mixing-minimum').textContent(),'— °C');


 await page.locator('#analysis-form [name=ambient_c]').fill('26');await refresh();assert.equal(await page.locator('#analysis-form [name=ambient_c]').inputValue(),'26');

 await page.getByText('Export a import parametrů pro vyhodnocení dat',{exact:true}).click();await page.locator('#analysis-export').click();const exported=JSON.parse(await page.locator('#analysis-json').inputValue());assert.equal(exported.parameters.ambient_c,25);exported.parameters.ambient_c=26.5;

 await page.locator('#analysis-json').fill(JSON.stringify(exported));await page.locator('#analysis-import').click();assert.equal(analysisConfig.parameters.ambient_c,25);assert.equal(await page.locator('#analysis-form [name=ambient_c]').inputValue(),'26.5');

 const thermostatRevision=s.settings.revision;await page.getByRole('button',{name:'Uložit model',exact:true}).click();await page.locator('#analysis-form .feedback').filter({hasText:'Model je uložený'}).waitFor();assert.equal(analysisConfig.parameters.ambient_c,26.5);assert.equal(s.settings.revision,thermostatRevision);

 await page.locator('#analysis-form [name=ambient_c]').fill('27');analysisConfig.revision++;s.analysis.config_revision=analysisConfig.revision;

 await refresh();await page.getByRole('button',{name:'Uložit model',exact:true}).click();await page.locator('#analysis-form .feedback.error').waitFor();assert.equal(analysisConfig.parameters.ambient_c,26.5);

 await page.getByRole('button',{name:'Načíst uložené parametry'}).click();await page.locator('#analysis-form .feedback').filter({hasText:'Načtené uložené'}).waitFor();

 exported.parameters.ambient_c='invalid';await page.locator('#analysis-json').fill(JSON.stringify(exported));await page.locator('#analysis-import').click();assert((await page.locator('#analysis-import-note').textContent()).includes('Neplatný parametr'));assert.equal(await page.locator('#analysis-form [name=ambient_c]').inputValue(),'26.5');

 await page.screenshot({path:path.join(out,'analysis-1440.png'),fullPage:true});

 await hub();await page.setViewportSize({width:375,height:900});await page.screenshot({path:path.join(out,'settings-375.png'),fullPage:true});await page.setViewportSize({width:1440,height:1050});await page.screenshot({path:path.join(out,'settings-1440.png'),fullPage:true});

 await openSettings('Wi-Fi a hotspot');assert.equal(await page.locator('[data-wifi-id]').count(),2);assert.equal(await page.locator('[data-wifi-id=ap]').getByText('Odebrat',{exact:true}).count(),0);

 await page.locator('[data-wifi-id="0"] [data-edit]').click();assert.equal(await page.locator('[name=wifi_0_password]').inputValue(),'');assert.equal(await page.locator('[name=wifi_0_ssid]').inputValue(),'Test WiFi');

 await page.locator('#add-wifi').click();await page.locator('[name=wifi_1_ssid]').fill('Backup');await page.locator('[name=wifi_1_password]').fill('new-test-password');await page.locator('[data-wifi-id="1"] [data-move="-1"]').click();await refresh();assert.equal(await page.locator('[name=wifi_1_password]').inputValue(),'new-test-password');assert.deepEqual(await page.locator('[data-wifi-id]').evaluateAll(rows=>rows.map(r=>r.dataset.wifiId)),['1','0','ap']);

 await openSettings('MQTT a Home Assistant');await page.locator('[name=mqtt_device_name]').fill('Termostat test');await page.locator('[name=mqtt_delta_c]').fill('0.25');await page.locator('[name=mqtt_publish_pin]').uncheck();await page.locator('#mqtt button.primary').click();await page.locator('#toast').filter({hasText:'Připojení uloženo'}).waitFor();

 assert.deepEqual(networkConfig.order,[1,0,'ap']);assert.equal(networkConfig.mqtt.device_name,'Termostat test');assert.equal(networkConfig.mqtt.delta_centi,25);assert.equal(networkConfig.mqtt.publish_pin,false);assert.equal(networkConfig.wifi[0].password_set,true);

 const posted=writes.findLast(w=>w.endpoint==='/api/network').body;assert(!Object.hasOwn(posted.wifi.find(w=>w.id===0),'password'));assert(!Object.hasOwn(posted.mqtt,'password'));assert(!Object.hasOwn(posted.ap,'password'));

 await unlock();await refresh();await page.locator('[name=mqtt_port]').fill('1884');networkConfig.revision++;await page.locator('#mqtt button.primary').click();await page.locator('#mqtt .feedback.error').waitFor();assert.equal(networkConfig.mqtt.port,1883);

 await page.locator('#mqtt button[type=reset]').click();await page.locator('#mqtt .feedback').filter({hasText:'Načtené uložené'}).waitFor();assert.equal(await page.locator('[name=mqtt_port]').inputValue(),'1883');

 await page.screenshot({path:path.join(out,'mqtt-1440.png'),fullPage:true});

 await openSettings('Wi-Fi a hotspot');await page.locator('#add-wifi').click();await page.locator('[name=wifi_2_ssid]').fill('New WiFi');await page.locator('#wifi button.primary').click();assert.equal(await page.locator('[name=wifi_2_password]').evaluate(e=>e.validity.valid),false);

 await page.locator('[data-wifi-id="2"]').getByText('Odebrat',{exact:true}).click();await page.locator('[data-wifi-id=ap] [data-edit]').click();await page.locator('#show-ap-key').click();await page.locator('#ap-key').filter({hasText:'test-only-ap-password'}).waitFor();await page.locator('#show-ap-key').click();assert(await page.locator('#ap-key').isHidden());

 await page.screenshot({path:path.join(out,'wifi-1440.png'),fullPage:true});await page.setViewportSize({width:375,height:900});await page.screenshot({path:path.join(out,'wifi-375.png'),fullPage:true});assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));await page.setViewportSize({width:1440,height:1050});

 await openSettings('OTA aktualizace');await page.locator('#ota-file').setInputFiles({name:'wrong.bin',mimeType:'application/octet-stream',buffer:Buffer.alloc(400)});await page.locator('#ota-form button.primary').click();await page.locator('#ota-form .feedback.error').waitFor();assert((await page.locator('#ota-form .feedback').textContent()).includes('není aplikační'));

 await page.locator('#ota-file').setInputFiles({name:'esp_termostat_solo.bin',mimeType:'application/octet-stream',buffer:otaBinary});await page.locator('#ota-form button.primary').click();await page.locator('#toast').filter({hasText:'Firmware'}).waitFor();assert.equal(await page.locator('#ota-progress').getAttribute('value'),'100');assert(otaPrepared);



 await openSettings('Displej');

 await page.locator('#display-form [name=display_source]').selectOption('outlet');

 await page.locator('#display-form [name=display_day_brightness]').fill('23');

 await refresh();

 assert.equal(await page.locator('#display-form [name=display_day_brightness]').inputValue(),'23','Polling overwrote draft');

 await page.getByRole('button',{name:'Uložit displej'}).click();

 await page.locator('#display-form .feedback').filter({hasText:'Nastavení je uložené'}).waitFor();

 assert.equal(s.settings.display_day_brightness,23);assert.equal(s.settings.display_source,'outlet');assert.equal(writes.at(-1).body.settings.display_off_centi,3000);

 await page.locator('[name=outlet_sensor]').selectOption('1');

 await page.getByRole('button',{name:'Uložit displej'}).click();await page.locator('#display-form .feedback').filter({hasText:'Nastavení je uložené'}).waitFor();

 assert.equal(s.settings.outlet_sensor,1);assert.equal(s.outlet,54.3);assert(Math.abs(s.difference+11.1)<.001);

 assert.equal(await page.locator('#sensor1-role').textContent(),'Výstup z kamen');

 assert.equal(await page.locator('[data-temp=probe1]').textContent(),'54,3');

 for(const rotation of [0,90,180,270]){

  await page.locator('[name=display_rotation]').selectOption(String(rotation));

  await page.getByRole('button',{name:'Uložit displej'}).click();await page.locator('#display-form .feedback').filter({hasText:'Nastavení je uložené'}).waitFor();

  assert.equal(s.settings.display_rotation,rotation);assert.equal(typeof writes.at(-1).body.settings.display_rotation,'number');

  assert.equal(await page.locator('#screen-preview').evaluate(n=>n.classList.contains('portrait')),rotation%180===90);

  assert.equal(await page.locator('#screen-preview .lcd-outlet span').textContent(),'54,3');

  assert.equal(await page.locator('#screen-preview .lcd-inlet span').textContent(),'65,4');

  assert.equal(await page.locator('#screen-preview .lcd-difference span').textContent(),'-11,1');

  if(rotation===0||rotation===90)await page.screenshot({path:path.join(out,`display-${rotation}-1440.png`),fullPage:true});

 }



 await page.locator('#display-form [name=display_day_brightness]').fill('42');s.settings.revision++;

 await refresh();await page.getByRole('button',{name:'Uložit displej'}).click();

 await page.locator('#display-form .feedback.error').waitFor();assert.equal(s.settings.display_day_brightness,23);

 await page.locator('#display-form').getByRole('button',{name:'Zahodit změny'}).click();assert.equal(await page.locator('[name=display_day_brightness]').inputValue(),'23');

 await openSettings('Senzory');await page.locator('[name=calibration1_centi]').fill('1.25');await page.locator('[name=calibration2_centi]').fill('-0.75');await page.getByRole('button',{name:'Uložit kalibrace'}).click();await page.locator('#sensors-form .feedback').filter({hasText:'Nastavení je uložené'}).waitFor();assert.equal(s.sensor1.calibration,1.25);assert.equal(s.sensor2.calibration,-.75);

 await page.locator('#back-overview').click();await page.getByRole('button',{name:'Spustit simulaci oběhu'}).click();await page.locator('#manual-remaining').filter({hasText:'600'}).waitFor();

 s.sensor1.valid=false;s.sensor2.valid=false;s.sensor1.temperature=null;s.sensor2.temperature=null;for(const key of ['inlet','outlet','average','minimum','maximum','difference'])s[key]=null;s.display.temperature=null;s.display.diagnostics=true;

 await refresh();await page.locator('#sensor-warning').waitFor();assert.equal(await page.locator('[data-temp=inlet]').first().textContent(),'—');

 s.sensor1.diagnostics={gpio:27,status:'no_presence',error:'ESP_ERR_NOT_FOUND',rom:null};s.sensor2.diagnostics={gpio:26,status:'warming_up',error:'ESP_OK',rom:'0123456789ABCD28'};

 await openSettings('Senzory');await refresh();await page.locator('#sensor2-info').filter({hasText:'tři po sobě'}).waitFor();assert((await page.locator('#sensor1-info').textContent()).includes('neodpovídá na reset'));assert((await page.locator('#sensor2-detail').textContent()).includes('0123456789ABCD28'));

 s.sensor2.diagnostics.status='updating';await refresh();await page.locator('#sensor2-info').filter({hasText:'pozastaveno během aktualizace'}).waitFor();

 s.sensor2.diagnostics.status='scratchpad_crc';await refresh();await page.locator('#sensor2-info').filter({hasText:'kontrolní součet'}).waitFor();

 await page.locator('#back-overview').click();

 await page.setViewportSize({width:375,height:900});await page.screenshot({path:path.join(out,'missing-sensors-375.png'),fullPage:true});

 offline=true;await refresh();await page.locator('#offline').waitFor();assert(await page.locator('#manual').isDisabled());assert(await page.locator('body').evaluate(n=>n.classList.contains('stale')));

 offline=false;await refresh();await page.waitForFunction(()=>document.querySelector('#connection').classList.contains('online'));

 await openSettings('Zařízení');await page.locator('#hostname').fill('ter01-test');await page.getByRole('button',{name:'Uložit název a restartovat'}).click();await page.locator('#new-address').waitFor();assert.equal(hostname,'ter01-test');assert.equal(await page.locator('#new-address a').getAttribute('href'),'http://ter01-test.local/');

 await page.locator('#back-overview').click();await page.locator('#settings-open').focus();await page.keyboard.press('Enter');assert((await page.locator('#nastaveni').isVisible())||(await page.locator('#unlock').isVisible()));
 await unlock();await page.clock.install();await page.clock.fastForward(301000);await page.locator('#unlock').waitFor();assert(await page.locator('#nastaveni').isHidden());

 assert.deepEqual(errors,[]);console.log('PASS: five responsive widths, compact overview/eight gated settings cards, login/idle expiry, ordered Wi-Fi/AP list/add/remove, Wi-Fi/MQTT credentials and revisions, OTA prepare/upload/identity/persistence; drafts survive polling; persisted saves, analysis model export/import/independent revision/draft/validation, stale revision rejection, separate calibrations, manual simulation, probe-role assignment, four rotations and LCD preview, null/offline states, hostname flow, keyboard navigation; no JS errors');

 }finally{await browser.close();server.close()}

})().catch(e=>{console.error(e);server.close();process.exitCode=1});

