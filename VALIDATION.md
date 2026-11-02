# Validation

## English/Czech interface — November 2, 2026

- `scripts/test_local.ps1`: all five C++ host suites passed, covering protocol behavior, pairing/recovery, radio-profile coordination, RTK simulation and radio-quality measurements.
- Web logic checks passed for signal classification, RTK freshness, explicit profile-switch confirmation, quality metrics, JavaScript syntax, DOM references and offline assets.
- Seven Chromium scenarios passed against the local simulator: switching preserves user text, author names, drafts, attachments and message actions; preferences survive reload; blocked storage works; offline status stays accurate; the selector remains accessible on mobile; an unsubmitted radio profile is retained and requires confirmation; server errors follow the selected language.

`pio run -e node_a -e node_b` passed for both firmware 1.6.1 targets, with the complete offline bilingual interface embedded. No physical board was flashed, no live radio range was tested, and no compiled firmware was published as part of this source update. The local build pair is for verification only and stays outside Git.

## Initial source import — November 1, 2026

All five C++ suites and web checks passed on Windows / MSVC 2022. Gitleaks 8.30.1 found no secrets in the exported source; examples used placeholders. A complete firmware build, board installation and live-device tests were not performed during that initial import.
