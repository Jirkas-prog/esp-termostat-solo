"""Package the SOLO build; never flash a connected device."""
import hashlib
import json
from pathlib import Path
import shutil
import sys

root = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1])
release = root / 'releases' / '0.8.5'
release.mkdir(parents=True, exist_ok=True)
meta = json.loads((out / 'flasher_args.json').read_text())
factory = bytearray(b'\xff' * max(int(a, 0) + (out / p).stat().st_size for a, p in meta['flash_files'].items()))
for address, source in meta['flash_files'].items():
    data = (out / source).read_bytes()
    start = int(address, 0)
    factory[start:start+len(data)] = data
    shutil.copy2(out / source, release / Path(source).name)
(release / 'factory.bin').write_bytes(factory)
shutil.copy2(out / 'dependencies.lock', release / 'dependencies.lock')
manifest = {'project': 'ESP TERMOSTAT SOLO', 'version': '0.8.5', 'chip': 'esp32',
            'board': 'LILYGO TTGO T-Display original ESP32', 'flash_size': '4MB',
            'factory_offset': '0x0', 'update_transport': 'OTA', 'ota_min_version': '0.5.3', 'output_default': 'none', 'status': 'BENCH TEST REQUIRED',
            'sha256': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in release.glob('*.bin')}}
manifest['source_sha256'] = {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
    for base in (root/'common', root/'firmware') for p in base.rglob('*')
    if p.is_file() and 'managed_components' not in p.parts and p.name not in ('sdkconfig','sdkconfig.old')}
(release/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
(release/'CTI-ME.txt').write_text(
    'ESP TERMOSTAT SOLO 0.8.5 / puvodni TTGO T-Display ESP32\n'
    'Lokalni web po pripojeni Wi-Fi: http://ter01.local/ (nazev lze zmenit ve webu).\n'
    'Web: Prehled a Nastaveni; Wi-Fi, hotspot, MQTT, senzory, displej a OTA v kartach Nastaveni.\n'
    'Nastaveni odemyka nahodny 4mistny PIN z LCD; relace konci po 5 minutach necinnosti.\n'
    'Wi-Fi: seraditelny seznam az 8 siti a polozka AP. MQTT: vychozi zmena 0.2 C / nejdele 60 s.\n'
    'Volitelny Overovaci PIN pres MQTT je u nove instalace vypnuty.\n'
    'Bez nastavene Wi-Fi: pripojit hotspot TERMO-xxxxxx podle udaju na LCD, otevrit http://192.168.4.1/.\n'
    'Vychozi rezim NONE: GPIO25=0, GPIO33=0. Zadny impulz ani sepnuty budic.\n'
    'Jde o logicky zakaz vystupu, nikoliv potvrzeni stavu pripojeneho kontaktu NC.\n'
    'GPIO27 = vstupni sonda, GPIO26 = vystupni sonda. DATA nejvyse 3.3 V!\n'
    'VDD sond = skutecny pin 3V3 desky. Puvodni 5V kontakt konektoru fyzicky prepojit a preznacit.\n'
    'Desku napajet pres USB/samostatny 5V vstup. GPIO27 zustava DATA, nikoliv napajeni HIGH.\n'
    'Prejdi nejprve README.md a docs/UVEDENI_DO_PROVOZU.md.\n'
    'Prvni instalace na stole: python -m esptool --chip esp32 --port COMx write_flash 0x0 factory.bin\n'
    'factory.bin PREPISUJE NVS: maze nastaveni, sit a vazby cidel. Port nejprve identifikuj.\n'
    'Dalsi aktualizace VZDY OTA: python tools/ota.py releases/0.8.5/esp_termostat_solo.bin\n'
    'Prechod ze starsi verze bez OTA: jednorazove USB podle docs/OTA.md, zachovat NVS.\n'
    'Pri aktualizaci aplikace se zachova i pripadne drive povoleny vystup!\n'
    'Prvni nahrani a overeny rozsah funkce jsou v docs/NAHRANI.md; provozni zkousky zustavaji nutne.\n', encoding='utf-8')
print(f'Packaged {release}')
