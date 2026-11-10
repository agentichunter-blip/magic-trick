#!/usr/bin/env bash
# Builds only. Never connects to a device or installs the larger partition table.
set -euo pipefail
project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${IDF_PATH:?Activate ESP-IDF v5.3 with its export.sh first}"
case "$(idf.py --version)" in
  "ESP-IDF v5.3"|"ESP-IDF v5.3-dirty") ;;
  *) echo 'This build requires ESP-IDF v5.3.' >&2; exit 1 ;;
esac
idf.py -C "$project_dir" -B "$project_dir/build-wifi" \
  -D SDKCONFIG="$project_dir/sdkconfig.wifi" \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.wifi.defaults' build size
if [[ "$(uname -s)" == Darwin ]]; then
  export SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"
fi
host_python="${CHROMAGIC_HOST_PYTHON:-python3}"
cmake -S "$project_dir/tests/host" -B "$project_dir/build-host-wifi" -G Ninja \
  -DPython3_EXECUTABLE="$(command -v "$host_python")"
cmake --build "$project_dir/build-host-wifi"
ctest --test-dir "$project_dir/build-host-wifi" --output-on-failure
printf '%s\n' 'Experimental build ready. NOT an app-only upgrade: it needs the Wi-Fi partition table and the fpga/cores images.'
