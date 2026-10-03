#!/bin/sh
# Host tests. Needs: brew install opus opus-tools
set -e
cd "$(dirname "$0")"
cc -O2 -Wall -o /tmp/test_ogg_opus test_ogg_opus.c ../main/audio/ogg_opus.c -I"$(brew --prefix opus)/include/opus" -L"$(brew --prefix opus)/lib" -lopus -lm
/tmp/test_ogg_opus /tmp/test_ogg_opus.ogg
opusinfo /tmp/test_ogg_opus.ogg | grep -qi "Playback length: 0m:02.99" || { opusinfo /tmp/test_ogg_opus.ogg; echo "FAIL: length"; exit 1; }
opusinfo /tmp/test_ogg_opus.ogg 2>&1 | grep -qiE "warning|error" && { opusinfo /tmp/test_ogg_opus.ogg; echo "FAIL: opusinfo warnings"; exit 1; }
opusdec --quiet /tmp/test_ogg_opus.ogg /tmp/test_ogg_opus.wav
echo "PASS: ogg_opus muxer"
cc -Wall -o /tmp/test_buttons test_buttons.c && /tmp/test_buttons
