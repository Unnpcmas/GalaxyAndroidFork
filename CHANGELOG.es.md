REGISTRO DE CAMBIOS · GALAXY ANDROID
Versión 0.0.38 (versionCode 43) · ARM64 (arm64-v8a) · Android 11+ (API 30)
Fork no oficial mantenido por Unncpmas

Este documento describe la compilación actual y sus áreas técnicas; no incluye el historial de versiones anteriores.

CAMBIOS DE 0.0.38
• El menú muestra v0.0.38 y el manifiesto usa versionCode 43, manteniendo el mismo paquete y certificado de firma.
• Se retiraron etiquetas provisionales de las opciones visibles.
• La depuración de Android está desactivada y la actividad del juego ya no se puede abrir desde otras apps.
• El primer arranque del motor debe confirmarse en el teléfono de destino antes de publicar esta compilación.


══════════════════════════════
1. ESTADO DE PESO Y TEMPERATURA
══════════════════════════════

Peso (APK e instalación)
• Librería estándar de Python recortada: de 758 a 382 archivos (6,16 MB → 2,78 MB comprimida). Se conserva todo lo que usa el convertidor del disco y el arranque de Python (incluida la compatibilidad con Android y los códecs de texto, entre ellos Shift-JIS, que se usa para leer nombres del disco). Se quitó lo que no se ejecuta nunca aquí: pruebas, documentación interna, perfilador, consola interactiva, red/correo/HTTP, módulos de prueba en C y los códecs de chino y coreano.
• Se quitaron tres librerías nativas de Python que nadie cargaba: libcrypto (4,5 MB), libssl (1 MB) y libsqlite3 (1,2 MB). Solo las usaban los módulos ssl, hashlib (OpenSSL) y sqlite3, que el convertidor no importa. hashlib sigue funcionando con las implementaciones internas.
• Ahorro estimado: unos 6 MB menos en el APK y unos 19 MB menos de espacio ocupado en el teléfono. El ahorro en el teléfono llega tras reinstalar la librería estándar (la app lo hace sola una vez, al cambiar la marca interna de extracción a _v2).
• El empaquetador ahora rechaza empaquetar si la librería estándar todavía incluye módulos que necesitan las librerías retiradas, para evitar un fallo oculto en tiempo de ejecución.
• Opciones de compilación nuevas, apagadas por defecto: GALAXY_COMPRESS_NATIVE=1 (comprime las librerías del juego dentro del APK; Android las extrae al instalar) y GALAXY_STRIP_SYMBOLS=1 (quita los nombres de funciones de libgame.so).
• Herramientas nuevas: tools/trim_pythonhome.py (recorta la librería estándar) y tools/verify_pythonhome.py (la prueba con un Python 3.15 real).
• Excepciones añadidas a .gitignore para CMakeLists.txt, decomp/LICENSE-Petari.txt y tools/installer-requirements.txt, que la regla *.txt estaba dejando fuera del repositorio.

Temperatura y consumo de GPU
• Calidad adaptativa sin “vaivén”: antes bajaba 3 niveles de FXAA al caer de ~46 fps y subía 1 nivel tras 30 s estables en 60, así que en un teléfono que no aguanta el nivel alto la calidad y la carga de GPU subían y bajaban sin parar. Ahora, si una subida falla en menos de 3 minutos, ese nivel pasa a ser un tope hasta que cambies la calidad. En una simulación de una hora los cambios bajaron de 159 a 9.
• Presentación Vulkan en modo FIFO (vsync) en lugar de MAILBOX. FIFO es el modo de bajo consumo y siempre está disponible; MAILBOX mantiene imágenes extra en vuelo y deja los fotogramas caer en cualquier punto del refresco, lo que con el limitador de FPS hace que carga y tiempos de fotograma varíen. Para restaurar MAILBOX, compila con -DGALAXY_VK_MAILBOX=1.
• Nota: estos dos cambios se probaron solo por simulación/lectura de código; no hay medición de temperatura en un teléfono. Si algo se comporta raro, el registro (galaxy_log.txt) muestra cada cambio de calidad.


Correcciones
• Randomizer: arreglado un cuelgue (hilo del juego al 100 % de CPU, “no frame presented for 8 s”) y un cierre inesperado por recursión infinita (SIGSEGV por desbordamiento de pila) al empezar partida con el randomizer encendido. Causa: las tablas del randomizer son estructuras que viven toda la ejecución, pero en el port cualquier “new” hecho desde un hilo del juego reserva memoria dentro del heap del propio juego (JKRHeap), que se destruye y reutiliza en cada cambio de escena. Los nodos de esas tablas quedaban pisados, y la siguiente búsqueda o limpieza seguía un árbol corrupto sin fin. Ahora todas las tablas del randomizer (puertas de galaxias, objetos, misiones, estadísticas de escena y las listas fijas de galaxias/familias) reservan su memoria directamente de libc, fuera del heap del juego. Comprobado con una prueba en PC: antes hacía 7.409 reservas del heap del juego en 200 reconstrucciones de tablas; ahora 0, y las pruebas del randomizer, ThreadSanitizer y AddressSanitizer pasan limpias.
• Audio/carga de nivel: arreglado un segundo cuelgue, justo al pulsar “Empezar” en la selección de archivo con el randomizer encendido (el juego se quedaba congelado, “no frame presented for 8 s”). Misma clase de fallo: la cola de mensajes del DSP de audio guardaba su memoria en el heap del juego, que se limpia al cambiar de escena; la cola se corrompía, el fallo se “recuperaba” sin soltar su cerrojo y el hilo de audio, que tiene la CPU emulada, bloqueaba todo el juego. La cola y el búfer de paquetes ahora usan memoria del sistema. Por la misma razón se protegieron la cola de lectura del disco y las listas de borrado diferido de texturas (OpenGL ES y Vulkan).
• Logros: arreglado un cierre inesperado (“OSPanic ... HeapMemoryWatcher”, abort) al cambiar de nivel después de jugar un rato. El contador de logros guardaba su mapa y sus listas en el heap del juego, que se reinicia en cada cambio de escena; ahora todas sus operaciones reservan memoria del sistema.

═══════════════
2. MOTOR Y PLATAFORMA
═══════════════
• Es un port nativo en C++ para ARM64, no un emulador: el código del juego reconstruido (carpeta decomp) se compila junto con una capa de plataforma Android.
• Ventana de memoria: el port usa un rango de direcciones fijo de 32 bits. En esta compilación, el lanzador intenta reservarlo al iniciar el motor sin usar wrap.sh ni depuración; si Android ya ocupa esa zona, el juego no arrancará. Confirma el inicio en el teléfono de destino antes de distribuirla.
• Capa de plataforma (platform/src): comandos gráficos GX (FIFO), mezclador de audio DSP, entrada de mandos (WPAD), sistema de archivos del disco (DVD) con capa de mods, guardado (NAND), temporizadores/hilos del sistema operativo, vídeo (VI) y decodificador de películas THP.
• Almacenamiento: datos privados de la app; las copias de guardado pueden vivir en /sdcard/GalaxyData. Conserva copias de seguridad propias antes de actualizar o restaurar.


══════════════
3. GRÁFICOS Y RENDER
══════════════
• Dos renderizadores: OpenGL ES 3.2 (predeterminado por compatibilidad) y Vulkan 1.0 (opcional; la compatibilidad debe verificarse por dispositivo). Si Vulkan se pierde dos veces seguidas, la app vuelve a OpenGL ES.
• Generadores de shaders propios para cada backend (gl_shadergen y vk_shadergen): traducen la configuración de dibujo del hardware original a shaders.
• Resolución interna (Ajustes > General): 480p, 720p, 1080p (predeterminada), 1440p y 4K (3840 px en el lado largo). En pantalla completa se adapta al formato del panel; si no, se muestra 16:9 con bandas.
• FXAA: niveles 0 (apagado) a 12 de suavizado de bordes. La calidad adaptativa puede bajarlo temporalmente si los FPS caen, y nunca lo sube por encima de lo que elegiste.
• Controlador de GPU personalizado: la app puede redirigir la carga del controlador Vulkan del sistema a uno elegido por el usuario (en libreserve.so).
• Compatibilidad Mali: hay ajustes específicos para evitar fallos conocidos de este tipo de GPU al liberar objetos de sincronización de presentación.
• Orientación: automática (sensor horizontal), horizontal o horizontal invertida.


═══════════
4. TEXTURAS
═══════════
• Las texturas del juego se decodifican en la CPU desde los formatos nativos de GX a RGBA de 8 bits por canal (en Vulkan, VK_FORMAT_R8G8B8A8_UNORM).
• Formatos soportados: I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8, CI4, CI8, CI14X2 y CMPR (compresión por bloques de tipo S3TC).
• Texturas con paleta (CI4/CI8/CI14X2): se aplican las tablas de colores (TLUT) del juego en el formato de paleta indicado.
• Mipmaps: el rango de nivel de detalle (LOD) de cada muestreador se limita a la cantidad real de mipmaps de esa textura.
• Filtrado lineal. Un formato desconocido se pinta en magenta para que el error sea visible en lugar de fallar en silencio.


══════
5. AUDIO
══════
• El flujo de audio del juego (AI DMA, estéreo de 16 bits, normalmente a 32 kHz) pasa por un remuestreador y un búfer en anillo; AAudio lo consume al ritmo del dispositivo.
• Se cuenta y registra cada vez que el audio se queda corto (underruns), útil para diagnosticar cortes.
• Pueden ocurrir interrupciones de audio según el dispositivo y la versión de Android.


══════════════════
6. ENTRADA Y CONTROLES
══════════════════
• Controles táctiles configurables (modo táctil completo, cruceta, botones): tamaño, opacidad y posición ajustables, también para el stick de cámara y el botón de reiniciar cámara.
• Mandos compatibles; en la pantalla de título la A física se empareja con B, como pide el aviso del juego.
• Sensor de movimiento: giroscopio para apuntar el puntero estrella y para mover la cámara, con inversión de ejes y sensibilidad.
• Puntero relativo (estilo trackpad) opcional. Vibración (háptica) opcional, apagada por defecto.


══════════════════════════
7. MODS Y OPCIONES DE JUEGO
══════════════════════════
• Trucos: volar, jugar como Luigi (se aplica al cargar el siguiente nivel), saltar cinemáticas, salud infinita, vidas infinitas y fragmentos de estrella infinitos, cámara libre (solo durante la sesión).
• Dificultad: daño de enemigos y de jefes de 1× a 4×, un golpe mortal, sin curación, y límite de salud en 3.
• Randomizer con semilla (000000–999999): la misma semilla da siempre el mismo juego. Permite mezclar galaxias, misiones, enemigos y objetos, y opcionalmente potenciadores; el modo desafío evita mezclar potenciadores para que el juego siga siendo completable.
• Velocidad del juego: 1×, 1,5×, 2×, 3× y 4×.
• Idioma del juego: automático, inglés, español, francés, alemán o italiano. Campo de visión (FOV) ajustable. Notificaciones de logros legibles.
• Mods de archivos: superposiciones opcionales de directorio en <raíz del juego>/mods/<id>/files/ que reemplazan archivos sin tocar los datos convertidos originales; se pueden activar y priorizar uno a uno.


════════════════════════
8. RENDIMIENTO Y DIAGNÓSTICO
════════════════════════
• Límite de FPS por reloj de CPU (sin espera activa): 60 FPS nativos; el Ahorro de batería fija 30 FPS y limita la resolución a 720p o menos (la elegida se restaura al apagarlo).
• La frecuencia de refresco del panel se ajusta al objetivo y no pasa de 60 Hz salvo avance rápido, para no gastar energía de más.
• Monitor de rendimiento en pantalla: apagado, FPS o estadísticas completas.
• Registro local (galaxy_log.txt): resumen de fotogramas cada 5 s de juego (promedio, mínimo, máximo y cuántos pasan de 20, 33 y 100 ms), línea del dispositivo cada 30 s (batería, temperatura, relojes de CPU, memoria libre) y resumen de la sesión al cerrar. Un informe de cierre inesperado conserva el contexto de la escena.


═══════════════════
9. INSTALADOR Y CONVERSIÓN
═══════════════════
• La app convierte tu propia copia del juego, obtenida legalmente, al formato que usa el motor. No incluye ROM, datos del juego ni partidas.
• Extracción del disco con nodtool (libnodtool.so); opción de omitir las películas para ahorrar unos 2,3 GB.
• Conversión con Python 3.15 embebido y los scripts de cocinado: contenedores RARC/Yaz0, modelos J3D, colisiones KCL, texto BMG y BCSV, interfaz LYT, partículas JPA, audio JAudio y películas THP. Corre en hilos, con barra de progreso en pantalla.
• También se pueden importar mods en .zip con la misma ruta de conversión.


════════════════
10. LÍMITES CONOCIDOS
════════════════
• La compatibilidad y el rendimiento pueden variar según el dispositivo.
• Vulkan puede dar parpadeo o cierres en ciertos equipos; usa OpenGL ES si pasa.
• Proyecto no oficial, sin afiliación con Nintendo. Créditos y licencias completos en Ajustes > “Leer licencias y avisos completos”.
