#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${ROOT_DIR}"

DB_PATH="${GB_DB:-./data/catalog.db}"
DEFAULTS_PATH="${GB_DEFAULTS:-./config/defaults.json}"
SYSTEMS_DIR="${GB_SYSTEMS_DIR:-./config/systems.d}"
SETTINGS_PATH="${GB_SETTINGS:-./config/user_settings.local.json}"
SCALE="${GB_SCALE:-4}"
LOG_PATH="${GB_LOG:-./data/local-ui.log}"

mkdir -p ./data

echo "=== gblibd scan ===" > "${LOG_PATH}"
./build/gblibd \
  --db "${DB_PATH}" \
  --defaults "${DEFAULTS_PATH}" \
  --systems-dir "${SYSTEMS_DIR}" 2>&1 | tee -a "${LOG_PATH}"

echo "=== gbshell sdl ===" | tee -a "${LOG_PATH}"
echo "log: ${LOG_PATH}" | tee -a "${LOG_PATH}"
./build/gbshell \
  --presenter sdl \
  --scale "${SCALE}" \
  --db "${DB_PATH}" \
  --defaults "${DEFAULTS_PATH}" \
  --systems-dir "${SYSTEMS_DIR}" \
  --settings "${SETTINGS_PATH}" 2>&1 | tee -a "${LOG_PATH}"
