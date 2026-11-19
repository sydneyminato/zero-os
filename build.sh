#!/bin/bash
set -e
clang -O3 -arch arm64 -arch x86_64 \
  -framework Cocoa -framework AudioToolbox -framework ImageIO \
  -x objective-c zero_core.c -o zero_os
file zero_os
./zero_os
