#!/bin/sh
# Builds and runs the PC preview of the UI (needs g++ and a prior `pio run`
# so LovyanGFX is present in .pio/libdeps). Output images go to ./out.
# Extra arguments are passed as compiler flags, e.g.
#   sh build.sh -DFORECAST_ROWS=4 -DCLOCK_24H=0
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/../.."
LGFX="$(ls -d "$ROOT"/.pio/libdeps/*/LovyanGFX/src | head -1)"
mkdir -p "$HERE/build" "$HERE/out"

# Dummy credentials: config.h requires them (normally injected from secrets.ini)
CXXFLAGS="-std=gnu++17 -O1 -w -DLGFX_SDL -D_POSIX_THREAD_SAFE_FUNCTIONS=200112L \
  -DWIFI_SSID=\"\" -DWIFI_PASSWORD=\"\" -DOTA_PASSWORD=\"\" -DHA_TOKEN=\"\" \
  -I$HERE/stubs -I$LGFX -I$ROOT/include -I$ROOT/src"

# LovyanGFX core (compiled once)
for f in LGFXBase lgfx_fonts lgfx_v1 lgfx_v1_panel; do
  [ -f "$HERE/build/$f.o" ] || g++ $CXXFLAGS -c "$LGFX/lgfx/v1/$f.cpp" -o "$HERE/build/$f.o"
done
# Bundled C sources (fonts, image decoders)
for c in $(find "$LGFX" -name "*.c"); do
  o="$HERE/build/c_$(basename "$c" .c).o"
  [ -f "$o" ] || gcc -O1 -w -I"$LGFX" -c "$c" -o "$o"
done

SRC="$HERE/preview.cpp $HERE/lgfx_extra.cpp $ROOT/src/ui.cpp $ROOT/src/icons.cpp $ROOT/src/weather.cpp"
g++ $CXXFLAGS "$@" $SRC "$HERE"/build/*.o -o "$HERE/build/preview"
"$HERE/build/preview" "$HERE/out"
python "$HERE/ppm2png.py" "$HERE"/out/*.ppm
rm -f "$HERE"/out/*.ppm
