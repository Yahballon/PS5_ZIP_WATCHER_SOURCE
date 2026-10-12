#!/usr/bin/env bash
set -euo pipefail
# Miniz 2.2.0, MIT; pinned to ensure repeatable dependency downloads.
BASE=https://raw.githubusercontent.com/richgel999/miniz/2.2.0
mkdir -p third_party/miniz
for f in miniz.c miniz.h miniz_common.h miniz_tdef.h miniz_tinfl.h miniz_zip.h miniz_tdef.c miniz_tinfl.c; do
  curl -fLSs --retry 3 "$BASE/$f" -o "third_party/miniz/$f"
done
printf '#pragma once\n#define MINIZ_EXPORT\n' > third_party/miniz/miniz_export.h
printf 'Downloaded miniz 2.2.0 sources to third_party/miniz/\n'
