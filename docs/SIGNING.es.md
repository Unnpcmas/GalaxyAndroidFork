# Firma de Galaxy Android

[English version](SIGNING.md)

Android solo permite instalar una actualización encima de otra si ambas usan el mismo certificado de firma. Si cambia el certificado, la actualización falla; desinstalar antes puede borrar datos locales y partidas.

## Identidad de firma

El proyecto conserva un certificado fijo para que las futuras versiones puedan actualizar la instalación existente. La huella SHA-256 pública del certificado es:

`33EF18776854BCCECAC8F032B0611DF34CAFF6978C53DCC18883CEB053779CBE`

La clave privada y su contraseña no se incluyen ni se publican en este repositorio. Guárdalas únicamente en una ubicación local segura o en el gestor de secretos. No subas el archivo de firma, su contraseña ni una copia codificada. La carpeta local `out/` está excluida de Git.

Cambiar el certificado impide actualizar las instalaciones existentes. Respalda la clave de forma segura y no la rotes sin planificar cómo migrar a quienes ya instalaron la aplicación.

## Compilación local

El empaquetador puede usar una clave local o una variable de entorno protegida:

```bash
python package_mobile_apk.py
```

`GALAXY_KEYSTORE` puede indicar la ubicación local. No escribas la clave ni su contraseña en el código, en el historial de comandos o en Git.

## GitHub Actions

El flujo de publicación puede leer estos nombres de secretos de Actions:

- `GALAXY_KEYSTORE_B64`
- `GALAXY_KEYSTORE_PASS`

Añade los valores solo mediante la configuración protegida de secretos de GitHub. No los pegues en documentación ni en commits. El empaquetador verifica el certificado antes y después de firmar.

## Verificar el APK actual

Con Android SDK Build Tools instaladas:

```bash
apksigner verify --print-certs downloads/GalaxyAndroid-v0.0.32-arm64.apk
adb install -r downloads/GalaxyAndroid-v0.0.32-arm64.apk
```
