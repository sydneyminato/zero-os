#!/bin/bash
set -e
cd "$(dirname "$0")"
clang -O2 -x objective-c -fobjc-arc zero_core.c -o zero_os \
  -framework Cocoa \
  -framework AudioToolbox \
  -framework ImageIO \
  -framework WebKit \
  -framework Virtualization
codesign --force --sign - --entitlements entitlements.plist ./zero_os
./zero_os &
