# Galaxy Android

**Fork no oficial, mantenido por Unncpmas.**
[Read this page in English](README.en.md)

Este repositorio es un fork de [Galaxy Android, de OpenPort Labs](https://github.com/OpenPort-Labs/Galaxy-Android). Unncpmas mantiene este fork, pero no se atribuye la autoría del código upstream ni del trabajo de decompilación y sus colaboradores. Consulta los [créditos](CREDITS.md).

## Descargas de la versión más reciente

- **APK Android ARM64, versión 0.0.38 (código 43):** [descargar GalaxyAndroid-v0.0.38-arm64.apk](https://github.com/Unnpcmas/GalaxyAndroidFork/releases/download/0.0.38/GalaxyAndroid-v0.0.38-arm64.apk)
- **Release de GitHub:** [Galaxy Android 0.0.38](https://github.com/Unnpcmas/GalaxyAndroidFork/releases/tag/0.0.38)
- **Código fuente completo (.zip):** [descargar Galaxy-Android-Source-0.0.38.zip](https://github.com/Unnpcmas/GalaxyAndroidFork/releases/download/0.0.38/Galaxy-Android-Source-0.0.38.zip)
- **Explorar los archivos del proyecto:** [árbol de código](https://github.com/Unnpcmas/GalaxyAndroidFork/tree/main)
- **Notas, fecha y comprobación SHA-256:** [LATEST_UPDATE.md](LATEST_UPDATE.md)

Se incluye solo el APK más reciente. El paquete está compilado para `arm64-v8a`; no contiene soporte de 32 bits.

## Sobre el proyecto

Galaxy Android es un port nativo para Android basado en código C++. No es un emulador. Esta rama contiene trabajo en curso para:

- Ejecución nativa ARM64 y opciones gráficas OpenGL ES/Vulkan.
- Controles táctiles configurables y compatibilidad con mandos.
- Ajustes locales de pantalla y renderizado.
- Diagnóstico local de arranque, gráficos, audio y errores.
- Funciones de progreso, guardado y carga que siguen en desarrollo.

La estabilidad y la compatibilidad dependen del dispositivo y de la compilación. La lista describe el código del proyecto; no garantiza que cada función esté completa o funcione igual en todos los teléfonos.

## Estructura del repositorio

- `decomp/`: código reconstruido del proyecto original. Sus muchos archivos pequeños forman parte de la compilación; no son copias del juego.
- `platform/` e `installer-mobile/`: el port Android, la interfaz y las herramientas nativas del instalador.
- `tools/`: utilidades de preparación y empaquetado.
- `docs/`: guías técnicas y de uso.

Se conservan los scripts de compilación e instalación que usa la documentación. Se retiró un script antiguo que apuntaba al repositorio upstream, no a este fork. Las guías principales, los créditos y las notas de versión están en español e inglés. El código y la documentación técnica heredada se mantienen en su idioma original para preservar su significado y atribución.

## Datos y derechos

- El APK y el código fuente del repositorio no incluyen una copia del juego, ROM, partidas ni datos extraídos.
- Usa únicamente una copia del juego obtenida legalmente y cumple las leyes aplicables en tu país.
- No subas aquí ROMs, imágenes de disco, recursos del juego, partidas, claves de firma ni otra información privada.
- Este proyecto no está afiliado, patrocinado ni aprobado por Nintendo. Las marcas y los derechos del juego pertenecen a sus titulares.
- Que el repositorio sea público o privado no sustituye las licencias ni concede derechos sobre material de terceros.

## Compilar desde el código fuente

Consulta la [guía de instalación en español](docs/INSTALLATION.es.md) y el [resumen técnico en español](docs/TECHNICAL.es.md). También están disponibles las [versiones originales en inglés](docs/README.en.md). La compilación requiere Android SDK/NDK, JDK y las herramientas indicadas allí. La clave de firma se mantiene local y no forma parte del repositorio ni del código fuente.

## Licencias y créditos

El código principal conserva la licencia GNU GPL v3.0 del proyecto upstream. Algunos componentes tienen licencias propias: revisa [`LICENSE`](LICENSE), [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) y [`CREDITS.md`](CREDITS.md) antes de reutilizar o redistribuir cualquier parte.

## Apoyar este fork

[Patreon de Unncpmas](https://www.patreon.com/c/unnpcmas/membership)
