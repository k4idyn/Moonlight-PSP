#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

echo "[smoke] validating release prerequisites"

required_files=(
  "Makefile"
  "README.md"
  "CHANGELOG.md"
  "INSTALL.md"
  "moonlight_me_helper/Makefile"
  "third_party/openh264/Makefile.psp"
  "src/main.c"
)

for path in "${required_files[@]}"; do
  if [[ ! -f "$path" ]]; then
    echo "[smoke] missing required file: $path" >&2
    exit 1
  fi
done

if ! command -v psp-config >/dev/null 2>&1; then
  echo "[smoke] psp-config not found in PATH" >&2
  exit 1
fi

if ! psp-config --pspsdk-path >/dev/null 2>&1; then
  echo "[smoke] psp-config is present but PSPSDK is not configured" >&2
  exit 1
fi

if ! grep -q '^## \[1\.2\.0\]' CHANGELOG.md; then
  echo "[smoke] changelog is missing the v1.2.0 release heading" >&2
  exit 1
fi

if ! grep -q 'v1\.2\.0' README.md; then
  echo "[smoke] README is missing v1.2.0 release labeling" >&2
  exit 1
fi

if ! grep -q '^RETAIL_BUILD[[:space:]]*?=[[:space:]]*1' Makefile; then
  echo "[smoke] Makefile no longer defaults to RETAIL_BUILD=1" >&2
  exit 1
fi

conflicts="$(git grep -n -I -E '^<<<<<<< |^=======|^>>>>>>> ' -- . ':(exclude)third_party/**' || true)"
if [[ -n "$conflicts" ]]; then
  echo "[smoke] unresolved merge conflict markers detected:" >&2
  echo "$conflicts" >&2
  exit 1
fi

echo "[smoke] OK"