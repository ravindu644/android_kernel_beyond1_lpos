#!/bin/bash
clear
export ARCH=arm64
export PLATFORM_VERSION=12
export ANDROID_MAJOR_VERSION=s
ln -s /usr/bin/python2.7 $HOME/python
export PATH=$HOME/:$HOME/toolchain/proton-clang-12/bin:$PATH

ARGS="
CC=clang
CROSS_COMPILE=aarch64-linux-gnu-
ARCH=arm64
LD=ld.lld
AR=llvm-ar
NM=llvm-nm
OBJCOPY=llvm-objcopy
OBJDUMP=llvm-objdump
READELF=llvm-readelf
OBJSIZE=llvm-size
STRIP=llvm-strip
LLVM_AR=llvm-ar
LLVM_DIS=llvm-dis
"

export exynos_defconfig=exynos9820-beyond1lte_defconfig

work_dir=$(pwd)
dt_tool=$work_dir/binaries
rm -rf out && mkdir out

dtb_img() {
	chmod +777 $dt_tool/* -R
	$dt_tool/mkdtimg cfg_create $work_dir/out/dtbo_beyond1.img $dt_tool/beyond1.cfg -d $work_dir/arch/arm64/boot/dts/samsung
	$dt_tool/mkdtimg cfg_create $work_dir/out/dtb_beyond1.img $dt_tool/exynos9820.cfg -d $work_dir/arch/arm64/boot/dts/exynos
	
	}

clean_build() {
    make ${ARGS} clean && make ${ARGS} mrproper
    make ${ARGS} $exynos_defconfig
    make ${ARGS} menuconfig
    make ${ARGS} -j$(nproc)
    dtb_img
    cp $work_dir/arch/arm64/boot/Image $work_dir/out
    echo "Task Finished !"    
}

dirty_build() {
    make ${ARGS} $exynos_defconfig
    make ${ARGS} menuconfig
    make ${ARGS} -j$(nproc)
    dtb_img
    cp $work_dir/arch/arm64/boot/Image $work_dir/out
    echo "Task Finished !"       
}

scratch() {
    make$ {ARGS} $exynos_defconfig
    make ${ARGS} menuconfig
}

dirty() {
    make ${ARGS} menuconfig
}

dtb () {
	make ${ARGS} $exynos_defconfig
	make dtbs
	dtb_img
	}
	
clean () {
	make ${ARGS} clean && make ${ARGS} mrproper
	}	

echo -e "Choose an Option : (1, 2, 3, 4)

1. Clean build
2. Dirty build
3. Build Kernel config from scratch
4. Build Kernel config from previous run
5. Make dtb
6. Clean the source

Your choice : "

read -r value
if [ "$value" == 1 ]; then
    clean_build
elif [ "$value" == 2 ]; then
    dirty_build
elif [ "$value" == 3 ]; then
    scratch
elif [ "$value" == 4 ]; then
    dirty
elif [ "$value" == 5 ]; then
    dtb
elif [ "$value" == 6 ]; then
    clean           
else
    echo "Invalid input"
fi

#to copy all the kernel modules (.ko) to "modules" folder.
mkdir -p modules
find . -type f -name "*.ko" -exec cp -n {} modules \;
echo "Module files copied to the 'modules' folder."
