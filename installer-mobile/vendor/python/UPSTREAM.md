# Android Python runtime

The bundled runtime is the official CPython 3.15.0 Android embeddable package
for `aarch64-linux-android`, downloaded from:

<https://www.python.org/ftp/python/3.15.0/python-3.15.0-aarch64-linux-android.tar.gz>

SHA-256:

`571e4cf4e0aee3d1ab54a52ca3d4e6845b85ec0bb327f7debfb873172f445dab`

The app embeds CPython locally to cook a user-selected disc image. The APK
contains the matching ARM64 `libpython3.15.so`, its required Python runtime
libraries, public headers used to build the JNI bridge, and the Python standard
library under `installer-mobile/assets/pythonhome.zip`. The runtime does not
add network permissions to the Android app.

The official CPython distribution's `LICENSE.txt` is retained in that standard
library archive. Do not replace the shared library, headers, and standard
library independently; they must come from the same CPython release.
