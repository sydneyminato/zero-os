#!/bin/bash
set -e
clang -O3 -arch arm64 \
  -framework Cocoa -framework AudioToolbox -framework ImageIO -framework Virtualization \
  -x objective-c zero_core.c -o zero_os
codesign --entitlements entitlements.plist -f -s - zero_os
file zero_os
./zero_os
