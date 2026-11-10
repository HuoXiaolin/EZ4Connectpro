#!/bin/bash
# Usage: ./scripts/macos-sign-app.sh "<app_path>"

set -euo pipefail

APP_PATH="${1:-}"

if [[ -z "${CODESIGN_IDENTITY:-}" ]]; then
  echo "CODESIGN_IDENTITY is required"
  exit 1
fi

if [[ -z "$APP_PATH" || ! -d "$APP_PATH" ]]; then
  echo "App not found: $APP_PATH"
  exit 1
fi
codesign --verbose --force --options runtime --deep --sign "$CODESIGN_IDENTITY" "$APP_PATH"
