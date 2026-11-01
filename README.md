# ESP LoRa Chat

Dvojice XIAO ESP32-S3 + Wio-SX1262: místní Wi-Fi chat, přenos souborů, LoRa/GFSK profily a měření propustnosti.

## Sestavení a nastavení

Nejdříve python scripts/create_secrets.py vytvoří NOVÝ soukromý klíč páru a heslo AP. Potom pio run -e node_a -e node_b. Obě desky musí používat stejný soukromý klíč. Existující klíč se negeneruje znovu. Testy: scripts/test_local.ps1. Firmware vytvořený s osobním klíčem nesdílejte; radio parametry nastavte podle místních pravidel.

## Obsah repozitáře

Zdrojové kódy, konfigurace sestavení a dostupné testy. Soukromé přístupy, záznamy instalací, zálohy zařízení a dříve sestavené binární soubory sem nepatří. Příklady vyplňte vlastními údaji až místně.

Toto je zdrojový snímek uložený 1. 11. 2026. Stav konkrétních kontrol je uveden v `VALIDATION.md`; nahrání zdrojů neznamená nové ověření na fyzickém zařízení.

## Další projekty

[Přehled projektů Jirkas-prog](https://github.com/Jirkas-prog) · [Home Assistant doplňky](https://github.com/Jirkas-prog/home-assistant-addons)
