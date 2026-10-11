#!/usr/bin/env bash
# Build and run doctest through CTest inside the GCC 16.2 toolchain.
set -euo pipefail
cd "$(dirname "$0")/.."

docker build -f docker/Dockerfile -t erikslund-pool-lb-build .
docker run --rm \
  -v "$PWD:/src:ro" \
  -v erikslund_pool_lb_test_build:/build \
  erikslund-pool-lb-build \
  bash -lc 'cmake -S /src -B /build -DCMAKE_BUILD_TYPE=Debug -DNATIVE_ARCH=OFF && cmake --build /build -j"$(nproc)" && ctest --test-dir /build --output-on-failure'

docker build -f tests/e2e/Dockerfile.tester -t erikslund-pool-lb-tester .
source_directory="$PWD"
if command -v cygpath >/dev/null 2>&1; then
  source_directory=$(cygpath -m "$PWD")
fi
MSYS_NO_PATHCONV=1 docker run --rm --network none --read-only --cap-drop ALL \
  --security-opt no-new-privileges --pids-limit 32 --memory 128m --cpus 1 \
  -v "$source_directory:/src:ro" --entrypoint python3 erikslund-pool-lb-tester \
  -B -m unittest discover -s /src/tests/e2e -p test_qualification.py

