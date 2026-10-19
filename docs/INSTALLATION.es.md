---
title: Galaxy Android - Guía de instalación y configuración
description: Requisitos, instalación del APK ARM64 y preparación local de los datos del juego.
tags:
  - android
  - instalación
  - configuración
  - galaxy-android
---

# Galaxy Android — Guía de instalación

[English version](INSTALLATION.md)

Esta guía explica cómo instalar Galaxy Android en un teléfono, tableta o dispositivo portátil Android.

> **Créditos:** La preparación de recursos se apoya en las herramientas de [GalaxyQuest](https://github.com/bigmak94/GalaxyQuest) y en el trabajo de reconstrucción de código de [Petari](https://github.com/SMGCommunity/Petari). Consulta también los [créditos del fork](../CREDITS.md).
>
> **Aviso legal:** Debes aportar tu propia copia del disco, obtenida legalmente. Este repositorio no distribuye archivos propietarios del juego ni datos extraídos.

## Requisitos

- Android 11 o posterior (API 30+).
- Procesador ARM de 64 bits (`arm64-v8a`).
- 3 GB de RAM como mínimo; se recomiendan 4 GB o más.
- Espacio libre aproximado: 2,5 GB sin cinemáticas o 5 GB con ellas.

## Método 1: preparar los datos en el teléfono

El APK unificado puede extraer y convertir localmente la imagen de disco; no hace falta un PC. El proceso requiere tu propia imagen legal.

1. Copia la imagen del disco a la carpeta `Download` del teléfono.
2. Formatos admitidos por el preparador: `.rvz`, `.wbfs`, `.iso`, `.wia`, `.ciso` y `.zip`.
3. Instala el [APK ARM64 v0.0.32](https://github.com/Unnpcmas/GalaxyAndroidFork/releases/download/0.0.32/GalaxyAndroid-v0.0.32-arm64.apk).
4. Abre Galaxy Android y concede el acceso a archivos que solicite para leer la imagen y guardar los datos en `/sdcard/GalaxyData`.
5. En la pantalla de preparación, elige la imagen. Activa la opción de incluir cinemáticas solo si quieres esos archivos.
6. Inicia la preparación y espera a que termine antes de cerrar la aplicación.

La duración depende del teléfono y del tamaño de la imagen. No borres los datos ni desinstales la aplicación como primer intento de recuperación.

## Método 2: preparar los datos en un PC

Este método es opcional. Necesitas Windows, macOS o Linux, Python 3.8 o posterior, Android Platform-Tools (`adb`) y una herramienta de extracción de discos compatible. **Dolphin y sus herramientas no se incluyen en este repositorio.**

1. Activa la depuración USB en las opciones de desarrollador y conecta el teléfono.
2. Extrae con tu herramienta local la copia legal del disco. Debes obtener una carpeta `extracted/DATA` con las carpetas `files` y `sys`.
3. Desde la raíz del proyecto, cocina los recursos:

   **Windows**
   ```cmd
   py tools\cook\cook.py extracted cooked --with-movies
   ```

   **macOS o Linux**
   ```bash
   python3 tools/cook/cook.py extracted cooked --with-movies
   ```

4. Transfiere los datos con `transfer_game.bat` en Windows o con ADB:
   ```bash
   adb push cooked/files /sdcard/GalaxyData/
   adb push cooked/sys /sdcard/GalaxyData/
   ```
5. Instala el APK incluido en el repositorio y abre la aplicación:
   ```bash
   adb install -r downloads/GalaxyAndroid-v0.0.32-arm64.apk
   adb shell am start -n com.openport.galaxyandroid/.MainActivity
   ```

## Solución de problemas

### Se cierra al iniciar o cargar

- Comprueba los permisos de archivos y que los datos estén en `/sdcard/GalaxyData`.
- La versión 0.0.32 es de desarrollo y no garantiza resolver todos los cierres de cada dispositivo. Si puedes, exporta el registro de diagnóstico y conserva una copia aparte de las partidas antes de reinstalar o cargar un estado.

### El puntero se mueve en dirección incorrecta

Abre los ajustes de controles y cambia **Invertir X** o **Invertir Y** si tu compilación ofrece esas opciones. El giroscopio varía según el teléfono.

### Rendimiento bajo

Si la compilación lo ofrece, baja la resolución de renderizado o activa el modo de ahorro de batería. El resultado depende del procesador y la GPU.

## Guías relacionadas

- [Controles y mandos](CONTROLS.es.md)
- [Resumen técnico](TECHNICAL.es.md)
- [Proceso de instalación desde PC](DESKTOP_INSTALLER.es.md)
- [Firma y conservación de la clave](SIGNING.es.md)
- [Petari](https://github.com/SMGCommunity/Petari) · [GalaxyQuest](https://github.com/bigmak94/GalaxyQuest)
