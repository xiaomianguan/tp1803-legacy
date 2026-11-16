#!/bin/bash
# Replace the tree's prebuilt WebView (CM 13: 100.0.4896.127, AOSPA: 44) with
# 106.0.5249.126, the last Chromium milestone that supports Android 6.0 (M107
# raised the minimum to API 24). usage: SRC=<source tree> fetch-webview-m106.sh
# APKs are LineageOS builds (com.android.webview, minSdk 23) from the
# backup/main_20230314 branches of LineageOS/android_external_chromium-webview_prebuilt_{arm,arm64}.
set -euo pipefail
SRC=${SRC:-$HOME/aospa-build/src}
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
fetch() { # arch commit sha256
  git init -q "$TMP/$1"
  git -C "$TMP/$1" remote add o "https://github.com/LineageOS/android_external_chromium-webview_prebuilt_$1"
  git -C "$TMP/$1" fetch -q --filter=blob:none o backup/main_20230314
  git -C "$TMP/$1" -c advice.detachedHead=false checkout -q "$2"
  echo "$3  $TMP/$1/webview.apk" | sha256sum -c -
  cp "$TMP/$1/webview.apk" "$SRC/external/chromium-webview/prebuilt/$1/webview.apk"
}
fetch arm64 93f703e8e0c08b230016d74f33d1625a0a50bd93 5a5671de57634476b95fe769b0a1836504258841d876509c07987b4d705706e2
fetch arm   4b0ae494a7dd7b8576c9d4fe1a0196d50a5d9a7b e2edc1f89ec1608b169da9e7ffc037190b70c7825f0be29694f4cae719980580
