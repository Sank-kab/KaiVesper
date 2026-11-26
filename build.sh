#!/usr/bin/env bash
# Build kaivesper.nro inside the ounce-devkit Docker image.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE="${OUNCE_DEVKIT_IMAGE:-ounce-devkit:latest}"

echo "Building kaivesper.nro using Docker image: $IMAGE"
docker run --rm -u "$(id -u):$(id -g)" -v "$SCRIPT_DIR:/src" -w /src "$IMAGE" make "$@"
echo "Done: $SCRIPT_DIR/kaivesper.nro"
