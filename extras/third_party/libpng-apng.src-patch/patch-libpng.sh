#!/usr/bin/env bash

set -e

if ! grep -q 'libpng version 1.6.58' libpng/png.h; then
	echo 'libpng source must be v1.6.58'
	exit 1
fi

if grep -q 'png_read_frame_head' libpng/png.h; then
	echo 'libpng already contains APNG changes'
else
	echo '--- Patching libpng with APNG support ---'
	patch -d libpng -p1 --forward < libpng-apng/libpng-1.6.58-apng.patch
fi

if ! grep -q 'png_read_frame_head' libpng/png.h; then
	echo 'libpng APNG patch did not provide the required API'
	exit 1
fi

if grep -q 'Release Library|x64' libpng/projects/vstudio/libpng/libpng.vcxproj; then
	echo 'libpng already contains 64-bit changes'
else
	echo '--- Patching libpng with 64-bit support ---'
	patch -d libpng -p1 --forward < libpng-x64.patch
fi
