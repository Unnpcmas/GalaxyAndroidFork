# Signing Galaxy Android

[Versión en español](SIGNING.es.md)

Android permits an update only when the installed APK and the new APK use the same signing certificate. A different certificate causes `INSTALL_FAILED_UPDATE_INCOMPATIBLE`; uninstalling first may remove local game data and saves.

## Signing identity

The project uses a fixed signing certificate so future builds can update existing installations. Its public SHA-256 certificate fingerprint is:

`33EF18776854BCCECAC8F032B0611DF34CAFF6978C53DCC18883CEB053779CBE`

The private keystore and its passphrase are not included in this repository or documented here. Keep both in a secure local location or the workspace secrets manager. Never commit a keystore, passphrase, or encoded key. The local `out/` directory is git-ignored.

Changing the signing certificate prevents in-place updates; a clean reinstall may erase local game data. Back up the keystore securely and do not rotate it without planning for existing installs.

## Local builds

The package script can use a locally stored key or a protected environment variable:

```bash
python package_mobile_apk.py
```

`GALAXY_KEYSTORE` can point to a local keystore. Do not put its contents or passphrase in command history, source files, or Git.

## GitHub Actions

The release workflow can read these repository Actions secret names:

- `GALAXY_KEYSTORE_B64`
- `GALAXY_KEYSTORE_PASS`

Add secret values only through GitHub's protected Secrets settings. Never paste the values into documentation or commit them. The packaging script checks the certificate before and after signing.

Experimental builds made with a different certificate are not updates for existing installs. Forks that intentionally use another certificate must set and verify their own signer fingerprint.

## Verify the current release

With Android SDK Build Tools installed:

```bash
apksigner verify --print-certs downloads/GalaxyAndroid-v0.0.32-arm64.apk
adb install -r downloads/GalaxyAndroid-v0.0.32-arm64.apk
```
