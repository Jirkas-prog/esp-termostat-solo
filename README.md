# ESP termostat SOLO

Samostatná varianta pro původní TTGO T-Display: dvě DS18B20 sondy, web, MQTT discovery a volitelný výstup.

## Sestavení a nastavení

ESP-IDF 5.3.1, cíl esp32. Host testy: tools/test.ps1; web: node tests/web.cjs. Výchozí režim none neovládá relé. Firmware je experimentální a nenahrazuje ochranu topného systému. OTA používá tools/ota.py včetně přípravného handshake; zachovejte NVS a kalibrace. Měřené trendy nejsou důkazem chodu čerpadla.

## Obsah repozitáře

Zdrojové kódy, konfigurace sestavení a dostupné testy. Soukromé přístupy, záznamy instalací, zálohy zařízení a dříve sestavené binární soubory sem nepatří. Příklady vyplňte vlastními údaji až místně.

Toto je zdrojový snímek uložený 1. 11. 2026. Stav konkrétních kontrol je uveden v `VALIDATION.md`; nahrání zdrojů neznamená nové ověření na fyzickém zařízení.

## Další projekty

[Přehled projektů Jirkas-prog](https://github.com/Jirkas-prog) · [Home Assistant doplňky](https://github.com/Jirkas-prog/home-assistant-addons)
