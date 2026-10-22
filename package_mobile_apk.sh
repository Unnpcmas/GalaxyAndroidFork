#!/usr/bin/env bash
# Keep one canonical APK packager. The Python packager adds the Java/Dex
# launcher, cooker runtime/assets, native libraries, licenses, alignment, and
# verifies the project's pinned signing certificate. A native-only zip is not
# a complete installable Galaxy Android app.
set -euo pipefail
cd "$(dirname "$0")"
source tools/env.sh

if [ -z "${ANDROID_HOME:-}" ] && [ -n "${ANDROID_SDK_ROOT:-}" ]; then
  export ANDROID_HOME="$ANDROID_SDK_ROOT"
fi
if [ -z "${ANDROID_HOME:-}" ]; then
  SDK="$(find_sdk)" || {
    echo "Android SDK not found; set ANDROID_HOME." >&2
    exit 1
  }
  export ANDROID_HOME="$SDK"
fi
if [ -z "${ANDROID_NDK_HOME:-}" ]; then
  NDK="$(find_ndk)" || {
    echo "Android NDK not found; set ANDROID_NDK_HOME." >&2
    exit 1
  }
  export ANDROID_NDK_HOME="$NDK"
fi

if ! command -v python3 >/dev/null 2>&1; then
  echo "Python 3 is required to package the complete Android APK." >&2
  exit 1
fi

exec python3 package_mobile_apk.py
