#!/bin/bash
# Builds an Android debug APK. Expects these variables:
#   ANDROID_HOME  the Android SDK, with the NDK below ANDROID_NDK (default: $ANDROID_HOME/ndk/27.2.12479018)
#   SDL_DIR       unpacked SDL2 sources
#   XZ_DIR        unpacked xz sources
#   OPENGFX_ZIP   opengfx-*-all.zip from https://cdn.openttd.org/opengfx-releases/
#   WORK_DIR      where builds go (default: ../android-build next to the checkout)
#   ABIS          space separated (default: arm64-v8a)
set -euo pipefail

SOURCE_DIR=$(cd "$(dirname "$0")/../.." && pwd)
ANDROID_NDK=${ANDROID_NDK:-$ANDROID_HOME/ndk/27.2.12479018}
WORK_DIR=${WORK_DIR:-$SOURCE_DIR/../android-build}
ABIS=${ABIS:-arm64-v8a}
mkdir -p "$WORK_DIR"
WORK_DIR=$(cd "$WORK_DIR" && pwd)

# Host tools generate sources during the cross-compile.
cmake -S "$SOURCE_DIR" -B "$WORK_DIR/tools" -G Ninja -DOPTION_TOOLS_ONLY=ON -DCMAKE_BUILD_TYPE=Release > /dev/null
ninja -C "$WORK_DIR/tools" tools > /dev/null

rm -rf "$WORK_DIR/jniLibs"
for ABI in $ABIS; do
	TOOLCHAIN=(-G Ninja "-DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake" "-DANDROID_ABI=$ABI" -DANDROID_PLATFORM=android-26 -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Release)
	PREFIX=$WORK_DIR/$ABI/prefix

	if [ ! -f "$PREFIX/lib/liblzma.a" ]; then
		cmake -S "$XZ_DIR" -B "$WORK_DIR/$ABI/xz" "${TOOLCHAIN[@]}" "-DCMAKE_INSTALL_PREFIX=$PREFIX" -DBUILD_SHARED_LIBS=OFF -DXZ_TOOL_XZ=OFF -DXZ_TOOL_XZDEC=OFF -DXZ_TOOL_LZMAINFO=OFF -DXZ_TOOL_LZMADEC=OFF -DCREATE_XZ_SYMLINKS=OFF -DCREATE_LZMA_SYMLINKS=OFF -DXZ_NLS=OFF > /dev/null
		ninja -C "$WORK_DIR/$ABI/xz" install > /dev/null
	fi
	if [ ! -f "$PREFIX/lib/libSDL2.so" ]; then
		cmake -S "$SDL_DIR" -B "$WORK_DIR/$ABI/sdl" "${TOOLCHAIN[@]}" "-DCMAKE_INSTALL_PREFIX=$PREFIX" -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF > /dev/null
		ninja -C "$WORK_DIR/$ABI/sdl" install > /dev/null
	fi

	cmake -S "$SOURCE_DIR" -B "$WORK_DIR/$ABI/game" "${TOOLCHAIN[@]}" "-DHOST_BINARY_DIR=$WORK_DIR/tools" -DOPTION_3D_PROOF=ON "-DCMAKE_PREFIX_PATH=$PREFIX" "-DCMAKE_FIND_ROOT_PATH=$PREFIX" '-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG -g0' ${CMAKE_EXTRA:-} > /dev/null
	ninja -C "$WORK_DIR/$ABI/game" -j "${JOBS:-3}" openttd

	mkdir -p "$WORK_DIR/jniLibs/$ABI"
	cp "$PREFIX/lib/libSDL2.so" "$WORK_DIR/$ABI/game/libmain.so" "$WORK_DIR/jniLibs/$ABI/"
	TRIPLE=$(case "$ABI" in arm64-v8a) echo aarch64-linux-android;; x86_64) echo x86_64-linux-android;; esac)
	cp "$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/$TRIPLE/libc++_shared.so" "$WORK_DIR/jniLibs/$ABI/"
done

# The game data is the same for every ABI.
ASSETS=$WORK_DIR/assets
GAME_BUILD=$WORK_DIR/${ABIS%% *}/game
rm -rf "$ASSETS"
mkdir -p "$ASSETS/game"
cp -r "$GAME_BUILD/lang" "$GAME_BUILD/baseset" "$GAME_BUILD/ai" "$GAME_BUILD/game" "$ASSETS/game/"
unzip -qo "$OPENGFX_ZIP" -d "$ASSETS/game/baseset"
cp "$SOURCE_DIR/os/android/openttd.cfg" "$ASSETS/openttd.cfg"

GRADLE=${GRADLE:-gradle}
"$GRADLE" -p "$SOURCE_DIR/os/android" --no-daemon -q assembleDebug \
	"-PsdlJavaDir=$SDL_DIR/android-project/app/src/main/java" \
	"-PjniLibsDir=$WORK_DIR/jniLibs" \
	"-PassetsDir=$ASSETS"
echo "$SOURCE_DIR/os/android/app/build/outputs/apk/debug/app-debug.apk"
