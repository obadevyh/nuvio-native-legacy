#!/bin/bash
# As pecas puras da atualizacao do fork (versionCode, anexo pelo sufixo + origem fixada + digest). Sem SDL.
#   bash tests/atualizacao_ota.sh
set -eu
cd "$(dirname "$0")/.."
cc -O1 -g -Wall -Wextra tests/atualizacao_ota.c -Isrc -o "${TMPDIR:-/tmp}/nuvio-atualizacao-ota"
"${TMPDIR:-/tmp}/nuvio-atualizacao-ota"
