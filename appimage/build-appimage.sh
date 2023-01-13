#! /bin/bash


set -x

set -e


# building in temporary directory to keep system clean

# use RAM disk if possible (as in: not building on CI system like Travis, and RAM disk is available)

if [ "$CI" == "" ] && [ -d /dev/shm ]; then

    TEMP_BASE=/dev/shm

else

    TEMP_BASE=/tmp

fi


BUILD_DIR=$(mktemp -d -p "$TEMP_BASE" appimage-build-XXXXXX)


# make sure to clean up build dir, even if errors occur

cleanup () {

    if [ -d "$BUILD_DIR" ]; then

        rm -rf "$BUILD_DIR"

    fi

}

trap cleanup EXIT


# store repo root as variable

REPO_ROOT=$(readlink -f $(dirname $(dirname $0)))

OLD_CWD=$(readlink -f .)


# switch to build dir

pushd "$BUILD_DIR"


# configure build files with meson

# we need to explicitly set the install prefix
meson "$REPO_ROOT" -Dprefix=/usr -Ddvbin=enabled -Dpkg_config_path=/usr/local/lib/pkgconfig -Dbuildtype=release -Dprefer_static=true -Dstrip=true -Djack=disabled

# build project and install files into AppDir
DESTDIR=AppDir ninja install
rm -r AppDir/usr/share/{man,metainfo,bash-completion,zsh,doc}

# lib quirks
mkdir -p AppDir/usr/local/lib/gconv
cp -a /usr/local/lib/gconv/aribb24 AppDir/usr/local/lib/gconv/

# now, build AppImage using linuxdeploy

# initialize AppDir, bundle shared libraries, add desktop file and icon, use Qt plugin to bundle additional resources, and build AppImage, all in one command

cp -a "$REPO_ROOT/appimage/"{apprun-hooks,fonts}  AppDir
linuxdeploy-x86_64.AppImage --appdir AppDir -e mpv -i "AppDir/usr/share/icons/hicolor/scalable/apps/mpv.svg" -d "AppDir/usr/share/applications/mpv.desktop" -l /usr/local/lib/libsobacas.so.0 -l /usr/local/lib/libyakisoba.so.0 -l /usr/lib/libpcsclite.so.1 --output appimage


# move built AppImage back into original CWD

mv mpv*.AppImage "$OLD_CWD"
