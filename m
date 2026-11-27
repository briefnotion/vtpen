#!/bin/sh
# Build vtprobe. "./m test" also builds and runs the vtpen tests (with sanitizers).
cd "$(dirname "$0")" || exit 1
g++ -std=c++17 -O2 -Wall -Wextra -Iinclude -o vtprobe tools/vtprobe.cpp || exit 1
echo "built: $PWD/vtprobe"
if [ "$1" = "test" ]; then
  g++ -std=c++17 -g -fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=all -Wall -Wextra -Iinclude -Itests -o vtpen_tests tests/vtpen_tests.cpp || exit 1
  ./vtpen_tests || exit 1
  g++ -std=c++17 -g -fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=all -Wall -Wextra -Iinclude -Itests -o vtpen_scene_tests tests/vtpen_scene_tests.cpp || exit 1
  ./vtpen_scene_tests
fi
