#!/bin/bash
set -e
clang -O3 -framework Cocoa -framework AudioToolbox -framework ImageIO -x objective-c zero_core.c -o zero_os
./zero_os
