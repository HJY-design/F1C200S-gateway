#!/bin/sh
set -eu
base=$(cd "$(dirname "$0")/../../.." && pwd)
python3 "$base/scripts/make-nand-images.py"
