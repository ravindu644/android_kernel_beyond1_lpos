#!/bin/bash

#Creating a symbolic link to avoid python issues.
ln -s /usr/bin/python2.7 "$HOME/python"

#exporting clang path
export PATH="$HOME/":"$HOME/toolchain/proton-clang-12/bin":$PATH

#saving current pwd as a variable
work_dir="$(pwd)"

#path for binary files
dt_tool="$work_dir/binaries"
repacker="$dt_tool/AIK/repackimg.sh"
AVBTOOL="$dt_tool/avbtool"
VBMETA="$dt_tool/addons/vbmeta.img"

#setting up executable permissions
sudo chmod +775 -R "$work_dir/binaries/"

#exporting variables
export DEVICE="S10"
export KBUILD_BUILD_USER="@ravindu644"
export ARGS="
ARCH=arm64
PLATFORM_VERSION=12
ANDROID_MAJOR_VERSION=s
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

#your defconfig
export exynos_defconfig=exynos9820-beyond1lte_defconfig

#size in bytes for boot and recovery images.
#beyond2

export BOOT_SIZE="57671680"
export RECOVERY_SIZE="67633152"

#cleaning output dir before building
rm -rf out && mkdir out

dtb_img() {
    sudo chmod +777 $dt_tool/* -R
    $dt_tool/mkdtimg cfg_create "$work_dir/out/dt.img" "$dt_tool/exynos9820.cfg" -d "$work_dir/arch/arm64/boot/dts/exynos"
}

packing() {
    echo -e "\n\n[+] Repacking boot.img..."
    cd "$dt_tool/AIK/ramdisk"
    if [ ! -d "debug_ramdisk" ]; then
        mkdir -p debug_ramdisk dev metadata mnt proc second_stage_resources sys
    fi
    cd "$work_dir"
    sudo bash "$repacker"
    echo -e "\n\n[+] Repacking Done..!"
    mv "$dt_tool/AIK/image-new.img" "$work_dir/out/boot.img"

    key() {
        if [ ! -d "$work_dir/binaries/key" ]; then
            mkdir "$work_dir/binaries/key"
        fi
        if [ ! -f "$work_dir/binaries/key/sign.pem" ]; then
            echo -e "\n\n[+] Generating a signing key.."
            openssl genrsa -f4 -out "$work_dir/binaries/key/sign.pem" 4096
        fi
    }
    key

    sign() {
        echo -e "\n\n[+] Signing New Boot image..."
        python3 "$AVBTOOL" extract_public_key --key "$work_dir/binaries/key/sign.pem" --output "$work_dir/binaries/key/sign.pub.bin"
        sudo chmod +777 "$work_dir/out/boot.img"
        python3 "$AVBTOOL" add_hash_footer --partition_name boot --partition_size "$BOOT_SIZE" --image "$work_dir/out/boot.img" --key "$work_dir/binaries/key/sign.pem" --algorithm SHA256_RSA4096
    }
    sign

    echo -e "\n\n[+] Signing Done..!"
    echo -e "\n\n[i] Creating a Flashable tar..!"

    cd "$work_dir/out"

    if [ ! -d "$DEVICE" ]; then
        mkdir "${DEVICE}"
    fi
    if [ ! -d "${DEVICE}/${SELINUX_STATUS}" ]; then
        mkdir "${DEVICE}/${SELINUX_STATUS}"
    fi
    tar -cvf "LPoS ${KERNEL_VERSION} [${DEVICE}] - ${SELINUX_STATUS}.tar" boot.img dt.img "${VBMETA}" ; rm boot.img dt.img
    mv "LPoS ${KERNEL_VERSION} [${DEVICE}] - ${SELINUX_STATUS}.tar" "${DEVICE}/${SELINUX_STATUS}"
}

tar_xz() {
    cd "$work_dir/out"
    tar -cvf "LPoS [${DEVICE}].tar" ./*
    xz -9 --threads=0 "LPoS [${DEVICE}].tar"
    mv "LPoS [${DEVICE}].tar.xz" "LPoS [${DEVICE}].xz"
    cd "$work_dir"
    echo -e "\n\n[i] Compilation Done..🌛"
}


checks() {
    if [ -f "$dt_tool/AIK/split_img/boot.img-kernel" ]; then
        echo -e "\n\n[i] Task Finished ! \n"
        packing
    else
        echo -e "\n\n[i] Build Failed :( \n"
        exit 1
    fi
}

permissive() {
    cd "$work_dir"
    config_file="arch/arm64/configs/$exynos_defconfig"

    replace_config_option() {
        sed -i "s/^$1=.*/$1=$2/" "$config_file"
    }

    # Modify configuration to enable SELinux permissive mode
    replace_config_option "CONFIG_SECURITY_SELINUX_ALWAYS_PERMISSIVE" "y"
    export SELINUX_STATUS="Permissive"

    # Perform dirty build
    dirty_build

    # Revert changes back to original configuration
    cd "$work_dir"
    replace_config_option "CONFIG_SECURITY_SELINUX_ALWAYS_PERMISSIVE" "n"
}


clean_build() {
    make ${ARGS} clean && make ${ARGS} mrproper
    make ${ARGS} "$exynos_defconfig"
    make ${ARGS} -j"$(nproc)"
    dtb_img
    mv "$work_dir/arch/arm64/boot/Image" "$dt_tool/AIK/split_img/boot.img-kernel"
    export SELINUX_STATUS="Enforcing"
    checks
    permissive
    tar_xz
}

dirty_build() {
    make ${ARGS} "$exynos_defconfig"
    make ${ARGS} -j"$(nproc)"
    dtb_img
    mv "$work_dir/arch/arm64/boot/Image" "$dt_tool/AIK/split_img/boot.img-kernel"
    checks
}

USER_INPUT=$1

if [ "$USER_INPUT" == "-c" ]; then
    echo -e "\n\n[i] Performing a clean build...\n\n"
    clean_build
elif [ "$USER_INPUT" == "-d" ]; then
    echo -e "\n\n[i] Performing a dirty build...\n\n"
    dirty_build
else
    echo -e "\n\n[x] Wrong Input..! \n\n [i] Usage : \n\n To a Clean build : build_kernel.sh -c\n To a dirty build : build_kernel.sh -d \n"
fi