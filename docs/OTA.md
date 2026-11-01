# Aktualizace SOLO přes Wi-Fi

Od 0.5.3 používej OTA, podle výslovného požadavku uživatele z 31. 10. 2026.
Výchozí adresa je `http://ter01.local`. Nahrává se samotná aplikace
`esp_termostat_solo.bin`, nikoliv `factory.bin`, bootloader nebo tabulka oddílů.

```powershell
python tools/ota.py releases/0.8.0/esp_termostat_solo.bin
```

Pro jiné jméno/IP přidej `--url http://192.0.2.58`. Skript používá standardní
Python bez dalších balíčků. Před přenosem ověří čip, projekt, verzi a SHA-256.
Po přenosu ověří změnu bootu a slotu, přesný ELF hash, potvrzený start a shodu
nastavení i obou kalibrací a od 0.6.0 také revizi a parametry analýzy. Síťová
konfigurace se nepřepisuje; od 0.7.0 ji skript také porovnává včetně revize. Klient posílá obraz po 4KB blocích s krátkou
pauzou a explicitní délkou HTTP těla; nepoužívá chunked encoding.

Od 0.7.0 je také **Nastavení → OTA aktualizace**. Web ověří hlavičku BIN, provede
přípravný handshake, zobrazí průběh a zkontroluje nový start, ELF identitu
a zachované nastavení. SHA obrazu ověřuje ESP32 před aktivací.
Sondy zůstávají fyzicky připojené. Skript před odesláním obrazu požádá o
přípravu: úlohy dokončí své transakce, zruší RMT kanály a označí měření jako
neplatné se stavem `updating`. GPIO zůstanou vstupní s pull-upem. Regulace
mezitím běží a kvůli neplatným měřením žádá ON; v režimu none jde o simulaci.
Po chybě se sběrnice znovu vytvoří a platnost se obnoví až po třech měřeních.
Po úspěšném zápisu se deska restartuje, což krátce přeruší měření, síť a software řízení.

Od 0.8 vyžaduje příprava i upload přihlášení PINem. Skript si PIN vyžádá
na displeji a přečte ho skrytě z terminálu. Volitelný `--session-file` ukládá
pouze krátkodobou relaci, nikdy PIN. V aplikaci se přihlašuje před vstupem
na kartu OTA. Po restartu se relace ukončí; otisky konfigurace z `/api/info`
umožní dokončit kontrolu beze změny oprávnění. Při první migraci ze 0.7 skript
požádá o nový PIN pro porovnání převedené konfigurace. Běžné další OTA porovnává
i SHA-256 otisky konfigurací. PIN a chybné pokusy zůstávají v NVS.

## Oddíly a návrat při selhání

| Oddíl | Offset | Délka |
|---|---:|---:|
| NVS | `0x9000` | `0x6000` |
| PHY | `0xf000` | `0x1000` |
| ota_0 | `0x10000` | `0x1e0000` |
| ota_1 | `0x1f0000` | `0x1e0000` |
| otadata | `0x3d0000` | `0x2000` |

Vše je uvnitř používaných 4 MB flash. Každý aplikační slot má 1 966 080 B.
Aktualizuje se neaktivní slot. Přerušený přenos se zahodí; neúspěšná kontrola
obrazu nepřepne boot. Mazání probíhá postupně po sektorech.

Bootloader má zapnutý rollback. Nová aplikace se potvrdí nejdříve po 10 s běhu,
když pracuje regulace, je uložené nastavení termostatu, sítě a přístupového PINu, připojená Wi-Fi nebo funkční AP, spuštěný web/LCD
a obě senzorové úlohy provedly alespoň tři pokusy. Platné teploty ani ACK z HA
nejsou podmínkou: porucha odpojeného čidla se nadále hlásí jako porucha čidla.
Pokud kontrola do 60 s neprojde, aplikace požádá o návrat k předchozí verzi.
Reset/pád před potvrzením vyvolá návrat při dalším startu. První USB instalace
po stejných kontrolách zaznamená sama sebe jako platný výchozí slot.
Princip vychází z [ESP-IDF OTA a rollbacku](https://docs.espressif.com/projects/esp-idf/en/v5.3.1/esp32/api-reference/system/ota.html).

Rollback chrání aplikaci; nemění uložený formát konfigurace. Budoucí migrace
nastavení proto musí zachovat kompatibilitu s předchozí verzí. OTA nemění
bootloader ani rozdělení flash. Tuto verzi lze bez změny NVS opakovaně přenést.

Firmware také ihned uzavírá odmítnuté POST přenosy a má rezervu síťových
socketů pro souběh otevřených webových stránek, OTA, MQTT a mDNS.

## Rozhraní

`GET /api/info` vrací objekt `ota` s `supported`, `max_size`, `slot`,
`pending_verify`, `rollback_available`, `prepare_required`, `version` a `elf_sha256`.
`POST /api/update/prepare` s prázdným tělem nejprve potvrdí `ready` po
uvolnění RMT obou sond. Teprve potom, do 10 s, klient odešle obraz.
`POST /api/update` přijímá binární tělo `application/octet-stream` a aktuální
hlavičku `X-Thermostat-Token` z `/api/info` a platnou relaci `X-Thermostat-Session`; obě vyžaduje i příprava.
Pokud klient po přípravě zmizí, měření se samo obnoví nejpozději za 150 s.
Přenos má limit 120 s a další návratové větve obnovují čtení hned.
Stejně jako nastavení používá
kontrolu Host a token proti požadavkům cizích webů, bez CORS. Jde o rozhraní
v důvěryhodné LAN s PIN přihlášením od 0.8; nejde o kryptografický podpis vydavatele.

Odmítá cizí čip/projekt, factory obraz, chybnou délku/SHA-256, verze před 0.5.3
a přenos při čekání na potvrzení startu/uložení nastavení. Přenos má limit 120 s
a limit čekání na data; po výpadku jej skript automaticky neopakuje naslepo.

## Jednorázový přechod z 0.4.x

Starý firmware OTA neobsahuje a má jediný aplikační oddíl. Zálohovat používanou
flash a NVS; ověřit identitu desky a zapsat přes USB pouze tyto soubory z aktuální verze:

```text
0x1000   bootloader.bin
0x8000   partition-table.bin
0x10000  esp_termostat_solo.bin
0x3d0000 ota_data_initial.bin
```

**Nezapisovat factory obraz a nemazat NVS.** Tabulka zachovává pozici i velikost
NVS a PHY. Po startu ověřit obě sondy, původní nastavení a síť. Pak provést
skutečné OTA skriptem, ověřit druhý slot a potvrzení běhu. Další aktualizace
už probíhají přes Wi-Fi; USB pouze pro výslovně domluvenou obnovu.

Na stole v režimu `none` lze před platným OTA ověřit odmítání vadných přenosů.
Test zapisuje neplatná data do neaktivního slotu; vždy po něm následuje platné
OTA, aby se obnovil platný rollback cíl:

```powershell
python tests/ota_live.py releases/0.8.0/esp_termostat_solo.bin
python tools/ota.py releases/0.8.0/esp_termostat_solo.bin
python tools/check_sensors.py --sensor 1 --samples 10
python tools/check_sensors.py --sensor 2 --samples 10
```
