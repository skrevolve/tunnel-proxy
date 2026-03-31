#!/usr/bin/env bash
# setup_chromium.sh
#
# Chromium 실행 파일을 탐지하거나 없으면 Chrome for Testing을 다운로드한다.
#
# 출력 (stdout): Chromium 실행 파일의 절대 경로
# 진행 메시지 : stderr (CMake execute_process가 stdout만 캡처하므로 분리)
#
# 탐색 순서:
#   1. 시스템 PATH — chromium-browser / chromium / google-chrome / google-chrome-stable
#   2. 이전 다운로드 캐시 — ~/.local/share/tunnel-proxy/chromium/chrome-linux64/chrome
#   3. Chrome for Testing 최신 stable 다운로드 (~300MB)

INSTALL_DIR="$HOME/.local/share/tunnel-proxy/chromium"
CHROME_BIN="$INSTALL_DIR/chrome-linux64/chrome"

# ── 1. 시스템 PATH 탐색 ────────────────────────────────────────────────────
for candidate in chromium-browser chromium google-chrome google-chrome-stable; do
    path=$(which "$candidate" 2>/dev/null || true)
    if [ -n "$path" ]; then
        echo "$path"
        exit 0
    fi
done

# ── 2. 캐시 확인 ──────────────────────────────────────────────────────────
if [ -x "$CHROME_BIN" ]; then
    echo "$CHROME_BIN"
    exit 0
fi

# ── 3. Chrome for Testing 다운로드 ────────────────────────────────────────
echo "[setup_chromium] Chromium not found in PATH. Downloading Chrome for Testing (~300MB)..." >&2

if ! command -v curl >/dev/null 2>&1; then
    echo "[setup_chromium] ERROR: curl not found. Install curl and retry." >&2
    exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "[setup_chromium] ERROR: python3 not found. Install python3 and retry." >&2
    exit 1
fi

if ! command -v unzip >/dev/null 2>&1; then
    echo "[setup_chromium] ERROR: unzip not found. Install unzip and retry." >&2
    exit 1
fi

# 최신 stable 버전 조회
VERSION=$(curl -sf \
    "https://googlechromelabs.github.io/chrome-for-testing/last-known-good-versions.json" \
    | python3 -c \
      "import sys,json; print(json.load(sys.stdin)['channels']['Stable']['version'])" \
    2>/dev/null || true)

if [ -z "$VERSION" ]; then
    echo "[setup_chromium] ERROR: Failed to fetch Chrome for Testing version (network issue?)" >&2
    exit 1
fi

echo "[setup_chromium] Version: $VERSION" >&2

mkdir -p "$INSTALL_DIR"

ZIP_URL="https://storage.googleapis.com/chrome-for-testing-public/${VERSION}/linux64/chrome-linux64.zip"
echo "[setup_chromium] URL: $ZIP_URL" >&2

if ! curl -Lf --progress-bar "$ZIP_URL" -o "$INSTALL_DIR/chrome-linux64.zip"; then
    echo "[setup_chromium] ERROR: Download failed." >&2
    rm -f "$INSTALL_DIR/chrome-linux64.zip"
    exit 1
fi

unzip -q "$INSTALL_DIR/chrome-linux64.zip" -d "$INSTALL_DIR"
rm -f "$INSTALL_DIR/chrome-linux64.zip"
chmod +x "$CHROME_BIN"

echo "[setup_chromium] Installed: $CHROME_BIN" >&2
echo "$CHROME_BIN"
