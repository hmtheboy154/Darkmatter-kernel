#!/bin/bash

echo "Test building kernel & modules script, attempt #3"
echo "Let's go :D" 
export ARCH=x86_64
export CROSS_COMPILE=

echo "Cleanup everything"
make clean && make mrproper
rm -rf $(pwd)/output

echo "Make output folder"
mkdir output

echo "Creating config"
cat arch/x86/configs/android-x86_64_defconfig /media/Work/nougat-x86/device/generic/common/selinux_diffconfig > $(pwd)/.config

echo "Start Compiling"
#make -j2 -C O=/media/Work/nougat-x86/kernel/out ARCH=x86_64 CROSS_COMPILE=\"/media/Work/nougat-x86/prebuilts/misc/linux-x86/ccache/ccache \"  olddefconfig

#make -j2 -C O=/media/Work/nougat-x86/kernel/out ARCH=x86_64 CROSS_COMPILE=\"/media/Work/nougat-x86/prebuilts/misc/linux-x86/ccache/ccache \"  bzImage modules

make -C $(pwd) olddefconfig
make -j4 -C $(pwd) bzImage modules
# make -j4 -C $(pwd) bzImage

echo "Copy the modules and bzImage out"

make INSTALL_MOD_PATH=$(pwd)/output modules_install 
cp arch/x86/boot/bzImage $(pwd)/output/kernel

echo "End, enjoy :D"
