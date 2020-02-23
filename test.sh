#!/bin/bash

echo "Test building kernel & modules script, attempt #2"
export ARCH=x86_64
export CROSS_COMPILE=

echo "cleanup everything"
make clean && make mrproper

echo "make output folder"
mkdir output

echo "creating config"
cat arch/x86/configs/android-x86_64_defconfig /media/Work/nougat-x86/device/generic/common/selinux_diffconfig > /media/Work/nougat-x86/kernel/.config

echo "compiling bzImage"
#make -j2 -C O=/media/Work/nougat-x86/kernel/out ARCH=x86_64 CROSS_COMPILE=\"/media/Work/nougat-x86/prebuilts/misc/linux-x86/ccache/ccache \"  olddefconfig

#make -j2 -C O=/media/Work/nougat-x86/kernel/out ARCH=x86_64 CROSS_COMPILE=\"/media/Work/nougat-x86/prebuilts/misc/linux-x86/ccache/ccache \"  bzImage modules

make -C $(pwd) O=output olddefconfig
make -j2 -C $(pwd) O=output bzImage modules
