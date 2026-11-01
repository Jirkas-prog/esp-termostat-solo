# Zapojení SOLO — nízké napětí a princip zálohy

## První fáze: dvě sondy, žádný budič ani relé

**Aktualizace: obě sondy se připojí přímo, bez adaptérů, a napájejí z 3,3 V.** Dosavadní vodič
konektoru vedoucí na 5V pin desky odpojit od 5 V, přepojit na skutečný pin **3V3**
a přeznačit kontakt. Nikdy nespojovat 5V a 3V3 větev dohromady. Tento konektor
potom poskytuje napájení sondám; už nepřivádí 5 V do desky.

Desku napájet regulovanými 5 V přes USB nebo její samostatný 5V/GND vstup.
Ověřit polaritu, jištění a dimenzování zdroje/kabelů. Při USB servisu nepřipojovat
dva nezávislé 5V zdroje proti sobě. Žádných 5 V na přeznačený 3V3 kontakt.
Ověřit napětí u sond i při současné konverzi a vysílání Wi-Fi; napájení DS18B20
musí zůstat v rozsahu datasheetu (nejméně 3,0 V).

| Sonda | VDD (červený) | GND (černý) | DQ (žlutý) |
|---|---|---|---|
| 1 — vstup do výměníku | 3V3 desky | GND | GPIO27 |
| 2 — výstup z výměníku | 3V3 desky | GND | GPIO26 |

Barvy odpovídají uvedenému produktu, ověřit skutečný kus. Každá sonda má vlastní
1-Wire sběrnici a **samostatný externí 4,7kΩ pull-up DQ → 3V3** uvnitř pouzdra.
Interní pull-up na GPIO27 i GPIO26 je od verze 0.2.0 na pokyn uživatele
**zapnutý** (`en_pull_up=true`). Externí 4,7kΩ odpory lze ponechat paralelně;
pro delší kabely zůstávají vhodné, interní pull-up je výrazně slabší.
GND je společná, parazitní napájení
se nepoužívá. U připojení sond vhodné blokování 100 nF VDD–GND.

**GPIO27 nenastavovat trvale na HIGH pro napájení.** V tomto firmwaru je to DQ
sondy 1 a při 1-Wire přenosu se stahuje k zemi. I jiný volný GPIO v HIGH je
logický výstup s úbytkem napětí a stavem závislým na bootu/resetu, nikoliv
stabilizovaný napájecí vývod. Používáme trvalou 3V3 větev regulátoru desky.
Napájení a klidovou úroveň DATA proměřit před připojením sond/desky.

Pokud musí původní 5V kontakt konektoru zůstat jako vstup napájení desky,
alternativou je samostatný regulátor 5 V → 3,3 V u sond, který napájí oba
adaptéry i jejich pull-up odpory; na GPIO27/26 dál vede jen DQ s úrovní 3,3 V.
Tato alternativa také nevyžaduje změnu firmwaru, ale není zakreslená jako hlavní
varianta.

GPIO25 a GPIO33 v první fázi ponechat bez budiče, zakončené odděleně. Firmware
je drží LOW i při poruchovém požadavku ON. To **není galvanické odpojení** ani
záruka rozepnutého NC kontaktu případného připojeného relé.

Sondy tepelně navázat na vstupní/výstupní potrubí výměníku, mechanicky zajistit
a vhodně izolovat od okolního vzduchu. DS18B20 není čidlo pro povrch ohniště ani
spaliny; jeho limit je 125 °C a konkrétní kabel/utěsnění může mít nižší limit.
Vést slaboproud odděleně od sítě a motorových vodičů. Délku sběrnice ověřit
osciloskopem a zkouškami rušení, neřešit ji pouze změnou softwarového timeoutu.

## Volitelná klávesnice TTP224

VCC 3V3, GND společná, momentový režim a aktivní HIGH. Q1=GPIO32, Q2=GPIO17,
Q3=GPIO36, Q4=GPIO39. Přidat 10kΩ pull-down každého vstupu na GND, zvlášť GPIO36/39
nemají interní pull-down. GPIO33 je nyní výhradně IN2 budiče; staré zapojení
klávesnice z dvojitého projektu se s ním nesmí spojit.

LCD: MOSI19, CLK18, CS5, DC16, RST23, BL4. Tyto piny se nemění. GPIO13 je
volitelný WDI výstup, překlápí se po 100 ms pouze po provedení regulační smyčky.

## Pozdější fáze: jeden můstek, jedno relé

Připravené řízení pro dvouvstupový H-můstek, například DRV8833 z projektu
`41_Vánoční světýlka`, s cívkou mezi AOUT1/AOUT2. VM=5 V pro **5V cívku**,
GND společná, AIN1=GPIO25, AIN2=GPIO33. **VM přivést samostatně ze stejného
5V zdroje; přeznačený 3V3 kontakt konektoru cívku nenapájí.** Každý vstup
opatřit 10kΩ pull-down.
Nepoužitý druhý můstek má oba vstupy LOW. Blokování VM podle datasheetu/modulu.
`nSLEEP` držet trvale HIGH přes **47kΩ na VM = 5 V**, pokud to již modul neřeší
(TI toto připojení přes odpor dovoluje v oddílu 7.3.4). Pro nSLEEP není potřeba
další GPIO; pro VM však po změně konektoru potřebujeme výše uvedený samostatný
5V přívod. Konkrétní modul je třeba proměřit.
Proudový limit a tepelný provoz musejí odpovídat cívce. Můstek nikdy nenapájí
síťové čerpadlo přímo.

| Režim | Požadavek ON | Požadavek OFF | Význam |
|---|---|---|---|
| `none` | IN1=0, IN2=0 | IN1=0, IN2=0 | měření/simulace, žádná energie do cívky |
| `nc` | 00 | 10 trvale | monostabilní 5V cívka, kontakt COM–NC |
| `no` | 10 trvale | 00 | monostabilní 5V cívka, kontakt COM–NO |
| `latching` | impulz 10 | impulz 01 | bistabilní jednocívkové, pak 00 |

U `nc` a `no` lze místo můstku použít 3,3V kompatibilní aktivní HIGH MOSFETový
budič s ochrannou diodou cívky; GPIO33 pak zůstává nepřipojené. Reléový modul
aktivní LOW do tabulky nepatří a bez další úpravy jej nepoužívat.

U bistabilního relé ověř polaritu SET=10 a RESET=01. Impulz 20–200 ms podle
datasheetu, výchozí 50 ms, nejvýše jeden za 2,1 s. Softwarový časovač ukončuje
impulz nezávisle na regulační úloze, ale úplné zamrznutí CPU vyžaduje hardwarové
omezení impulzu. Bistabilní relé při výpadku samo nepřejde na ON.
Původně zmíněné **PE014F12 má 12V cívku** a není 5V relé. DRV8833 má maximum
napájení 10,8 V, proto tuto 12V kombinaci nepoužít; vyžaduje jiný budič/napájení.

Pro monostabilní variantu je preferovaný COM–NC: při LOW budiče nebo ztrátě 5 V
je kontakt zavřený. Nejde o nezávislost na mechanickém zadření kontaktu.
Externí watchdog může odpojovat buzení NC cívky při ztrátě WDI; v této verzi
není připojen jeho stavový vstup ani zpětná vazba relé/průtoku.

## Mechanický termostat a silová část

V první fázi ponechat stávající mechanickou regulaci plně funkční. Později lze
elektronický kontakt zapojit paralelně s mechanickým **spínacím teplotním
kontaktem**, aby čerpadlo spustil kterýkoliv z nich. Elektronika jej nesmí
sériově blokovat. Pro tento způsob není vhodný obyčejný rozpínací havarijní
kontakt bez odpovídajícího dalšího zapojení.

Obě paralelní cesty musejí být ze stejného jištěného napájecího přívodu čerpadla,
ne ze dvou fází či různých záložních zdrojů. Kontakty musí jednotlivě zvládnout
motorový rozběhový proud. N/PE a ochrany podle skutečné instalace; síťové zapojení
má dokončit a ověřit kvalifikovaný elektrikář. Výkres je princip, nikoliv
schválený silový výrobní podklad; typ čerpadla a relé zatím není určený.

## Podklady

- [DS18B20 datasheet, str. 2 a 7](https://www.analog.com/media/en/technical-documentation/data-sheets/ds18b20.pdf): VDD, VPU, VIH při externím napájení, CRC, konverze.
- [ESP32 datasheet](https://www.espressif.com/sites/default/files/documentation/esp32_datasheet_en.pdf): napěťové domény, GPIO, vstupy 36/39.
- [Použitá sonda Drátek](https://dratek.cz/arduino-platforma/3068-teplomer-vodotesny-sonda-ds18b20-adapter-pro-arduino.html).
- [TI DRV8833 datasheet](https://www.ti.com/lit/ds/symlink/drv8833.pdf): rozsah VM, nSLEEP, tabulka vstupů, blokování.
- Lokální reference: `38_IRrem/ir-mqtt/platformio.ini`, `38_IRrem/gateway/main/display.c`, `41_Vánoční světýlka/README.md`.
- Ve fontu `firmware/main/font8x8_basic.h` je zachována původní public-domain hlavička.


Od verze 0.4.0 lze výstupní sondu vybrat v nastavení. Tabulka i výkres ukazují
výchozí přiřazení (sonda 1 / GPIO27 vstup, sonda 2 / GPIO26 výstup).
Při prohození rolí se nemění elektrické zapojení ani kalibrace fyzických sond.
