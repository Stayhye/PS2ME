#!/bin/sh
# Compila o projeto dentro do container ps2dev/ps2dev.
# Uso: ./build.sh [clean]
# O diretorio do projeto e montado em /src; PS2SDK/PS2DEV ja estao na imagem.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
# rsdk-ps2-builder = imagem ps2dev + make + gsKit ja compilados
IMAGE="${PS2_IMAGE:-rsdk-ps2-builder:latest}"
docker run --rm -v "$HERE":/src -w /src "$IMAGE" sh -c "make ${1:-all}"
