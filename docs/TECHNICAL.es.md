---
title: Galaxy Android - Resumen técnico
description: Resumen en español de la arquitectura del port Android ARM64 y sus límites actuales.
tags:
  - android
  - arm64
  - arquitectura
---

# Galaxy Android — Resumen técnico

[Detailed technical documentation in English](TECHNICAL.md)

Este resumen describe el diseño del port; no promete compatibilidad universal ni que todas las funciones estén completas.

## Arquitectura

Galaxy Android es un port para Android ARM64 basado en código C++. No es un emulador. La aplicación utiliza componentes nativos de Android y una capa de plataforma para pantalla, entrada, audio, almacenamiento y renderizado. La reserva de direcciones de memoria se implementa desde código nativo, incluido [`platform/android/reserve.c`](../platform/android/reserve.c).

## Gráficos y pantalla

El port incluye renderizadores OpenGL ES y Vulkan. En este fork, OpenGL ES es el valor predeterminado actual; Vulkan se conserva como opción, pero su compatibilidad debe verificarse por dispositivo. Hubo reportes de parpadeo y cierres en ciertos equipos. La resolución y el rendimiento dependen de la GPU, el controlador y la escena; no se garantizan cifras universales.

## Entrada y audio

La capa Android traduce eventos táctiles, mandos compatibles y sensores de movimiento a las entradas del juego. Las asignaciones y ajustes pueden variar entre compilaciones. El audio usa una ruta nativa de Android, pero puede presentar interrupciones según el dispositivo y la versión.

## Datos y partidas

La aplicación usa almacenamiento privado y puede mantener copias bajo `/sdcard/GalaxyData`. Las funciones de guardado y restauración siguen en desarrollo. Una copia de archivo o un periodo sin actividad no demuestra que el juego haya completado un guardado; la captura debe seguir una señal explícita de guardado terminado. Haz respaldos independientes y no desinstales para intentar recuperar datos sin conservarlos antes.

## Diagnóstico y rendimiento

Usa el registro local de la aplicación para investigar cierres, errores gráficos, audio y cargas. El consumo, la temperatura y los FPS cambian con el dispositivo y el contenido mostrado; las mediciones de un teléfono no predicen las de otro.

## Guías relacionadas

- [Instalación](INSTALLATION.es.md)
- [Controles y mandos](CONTROLS.es.md)
- [Firma del APK](SIGNING.es.md)
