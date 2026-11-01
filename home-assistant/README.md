# Home Assistant — SOLO přes Wi-Fi/MQTT

Od 0.8 lze v MQTT nastavení zapnout textový senzor **Ověřovací PIN**.
Jeho retained hodnota je v `wifi2mqtt/termostat-solo/access`, klíč
`verification_pin`. Vypnutí odstraní tuto zprávu i Discovery definici.
Teploty se výchozím nastavením posílají při změně 0,2 °C, nejdéle po 60 s.

Od 0.7.0 je výchozí **MQTT Discovery**: jedno zařízení **Termostat ter01** s 24
entitami, s povoleným PINem od 0.8.0 má 25 entit. HA musí používat stejný broker a zapnuté Discovery se stejným prefixem.
Webové **Nastavení → MQTT a Home Assistant** obsahuje přístupy, téma, jméno i
Discovery. Viz [síťová dokumentace](../docs/SIT_A_MQTT.md).

Dříve se publikovala pouze telemetrie, proto se při funkčním spojení nemuselo
objevit zařízení v registru HA. Pro explicitní potvrzování doručení z HA je
samostatný package [ack.yaml](ack.yaml). Bez něj zůstává bit 64 a ha_delivery=false.
Discovery potvrzení nesimuluje. Stejnou ACK automatizaci neinstalovat dvakrát.

## Původní ruční integrace — alternativa při vypnutém Discovery


Ruční YAML používá expiraci 190 s pro výchozí maximum přenosu 60 s. Při změně
maxima upravit i `expire_after` a `off_delay` na `3× maximum + 10 s`; Discovery
to aktualizuje automaticky.

Balíček `termostat_solo.yaml` vložit do `packages/` a sloučit s existujícím
nastavením balíčků. Nevytvářet druhý klíč `homeassistant`:

```yaml
homeassistant:
  packages: !include_dir_named packages
```

Před restartem provést kontrolu konfigurace skutečného HA. V této práci se
balíček do tvého HA nenahrával. Připojená deska má Wi-Fi a účet brokeru nastavené
podle světýlek / IR remote a telemetrie na brokeru byla ověřená. Pro další desku použít hotspot podle [postupu instalace](../docs/UVEDENI_DO_PROVOZU.md).

Kořen témat je `wifi2mqtt`, společný se světýlky a IR remote. SOLO používá vlastní
větev `wifi2mqtt/termostat-solo`; `wifi2mqtt/state` a `wifi2mqtt/trigger` patří IR remote.
Lokální web je od 0.3.0 na `http://ter01.local/`. Změna hostname přes web
nemění větev MQTT ani klientské ID; konfiguraci HA není kvůli názvu třeba měnit.

| Téma | Obsah |
|---|---|
| `wifi2mqtt/termostat-solo/state` | JSON každých 5 s / změna závady; QoS1, bez retain |
| `wifi2mqtt/termostat-solo/event` | snímek při změně závad, bez retain |
| `wifi2mqtt/termostat-solo/availability` | retained `online`, LWT `offline` |
| `wifi2mqtt/termostat-solo/ack` | sequence potvrzené z HA; bez retain |

Témata, ID entit a MQTT klient `kamna-termostat-solo` jsou odlišné od původního
redundantního projektu. Jeden takový klient může být připojen pouze jednou;
další desky od 0.7.0 dostávají vlastní ID a větev témat automaticky a lze je upravit ve webu.

Balíček vytváří vstup, výstup, průměr, min, max, diferenci, diagnostickou entitu
s JSON atributy, indikaci požadavku oběhu, povoleného výstupu, závad a spojení.
Hodnoty bez nové zprávy po 20 s expirují. Chybné čidlo je nedostupné, žádná
teplotní entita nedostane náhradní nulu. U diference není `device_class:
temperature`, protože jde o rozdíl a nikoliv absolutní teplotu.

`pump_request=true` v režimu `none` znamená **simulovaný požadavek**.
`output_enabled=false`, `active_relay_mode=none`, `command_sent=null` zůstávají
explicitní. `command_sent=true` u aktivního výstupu nedokazuje skutečný chod
čerpadla. Požadovaný uložený typ je zvlášť v `settings.relay_mode`; skutečně
používaný typ je `active_relay_mode`, protože změna vyžaduje restart.

Všechny teploty jsou v °C, `difference=outlet−inlet`. Agregace vyžadují obě
platná čidla a vznikají až po aplikaci individuálních kalibrací. `sensor1/2`
obsahují platnost, raw teplotu, kalibraci a stáří. `manual_remaining_s` ukazuje
zbytek ručního požadavku. Parametry, počítadla a maska závad jsou v diagnostice.

ACK automatizace potvrzuje doručení do HA; bez ní bude hlášen bit 64 závady,
i když broker připojení přijal. To může znamenat problém AP, brokeru, HA nebo
ACK automatizace — samo o sobě to neurčuje vadné rádio. Závada komunikace
neovlivňuje místní spínací logiku. Zprávy state/event/ack nikdo nesmí měnit
na retained; stará hodnota není důkaz živosti.

Nastavení a ruční ON jsou v této verzi přes LCD/TTP224 nebo USB, síťové řízení
čerpadla není součástí balíčku. Wi-Fi cesta slouží k telemetrii a dohledu.
Změny masky a nedostupnost se zapisují do Logbooku, teploty může uchovávat
Recorder. Zkontrolovat, že entity nejsou vyloučené. Offline čítač v RAM není
úplný trvalý deník a při restartu se vynuluje.

Podklady: [MQTT sensor](https://www.home-assistant.io/integrations/sensor.mqtt/)
a [MQTT binary sensor](https://www.home-assistant.io/integrations/binary_sensor.mqtt/).


Od SOLO 0.2.0 obsahuje JSON také `settings.display_*` a `display`: zvolenou
teplotu, noční režim, diagnostiku, platnost času, místní minutu dne, stav LCD
ovladače a nastavený jas. Současná diagnostická entita přebírá celý JSON,
takže změna nevyžaduje další entity ani jiné MQTT téma.


Od 0.4.0 odpovídají `inlet`/`outlet` přiřazení výstupní sondy ve webu;
`difference` je výstup minus vstup. `sensor1`/`sensor2` nadále označují fyzická
čidla GPIO27/26 včetně kalibrace, platnosti a závady; obsahují navíc kalibrovanou
`temperature` a `role`. ID existujících HA entit se nemění, názvy závad jsou
upravené na čísla fyzických sond.
