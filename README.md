# ESP LoRa Chat

An encrypted peer-to-peer messenger for two ESP32-S3 boards with Wio-SX1262 radios. Each board serves a local web interface for text, photos and files, with radio diagnostics and link tests. Chat works without internet access.

## English and Czech

Select **EN** or **CZ** in the header. The choice is remembered in this browser. A first visit follows a Czech browser preference and otherwise uses English. Switching remains available when browser storage is disabled.

Your author name, message contents, draft, selected attachment and unsubmitted radio profile remain unchanged. Interface labels, transfer states, diagnostics, radio-profile hints and error messages use the selected language.

## Build and configuration

Create a new private pair only when needed:

```sh
python scripts/create_secrets.py
pio run -e node_a -e node_b
```

The generator preserves an existing key. Both boards must use the same private pairing key; retain the deployed pair's key when preparing updates. Never share credentials or firmware built with your personal key. Configure radio operation in accordance with local rules.

The current source version is **1.6.1**. The web interface and both language dictionaries are embedded in the firmware and require no remote translation service or assets.

## Tests

On Windows with Visual Studio C++ tools, `scripts/test_local.ps1` runs the five host suites and web logic checks. Browser tests use a local simulator:

```sh
npm ci
npx playwright install chromium
npm run test:web
```

The RTK feature simulates traffic; it does not provide GPS measurements or establish positioning accuracy. See [VALIDATION.md](VALIDATION.md) for the checks performed and their limits.

## Repository contents

Source code, build configuration and tests. Credentials, device history, installation records, private backups and previously compiled firmware stay outside Git. Fill in your settings locally.

[Jirkas-prog project directory](https://github.com/Jirkas-prog) · [Home Assistant add-ons](https://github.com/Jirkas-prog/home-assistant-addons)
