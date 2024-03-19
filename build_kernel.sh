#!/bin/bash

#Creating a symbolic link to avoid python issues.
ln -s /usr/bin/python2.7 "$HOME/python"

#exporting clang path
export PATH="$HOME/":"$HOME/toolchain/proton-clang-12/bin":$PATH

#saving current pwd as a variable
export work_dir="$(pwd)"

#path for binary files
export dt_tool="$work_dir/binaries"
export repacker="$dt_tool/AIK/repackimg.sh"
export VBMETA="$dt_tool/addons/vbmeta.img"

#setting up executable permissions
sudo chmod +775 -R "$work_dir/binaries/"

#creating out folder
mkdir out || true 

#exporting variables
export current_datetime=$(date +"%Y-%m-%d_%H-%M-%S")
#export LPOS_KERNEL_VERSION="v8.7.1"
export DEVICE="S10+"
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
export config_file="arch/arm64/configs/$exynos_defconfig"

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

    echo -e "\n\n[i] Creating a Flashable tar..!"

    cd "$work_dir/out"

    if [ ! -d "$DEVICE" ]; then
        mkdir "${DEVICE}"
    fi
    if [ ! -d "${DEVICE}/${SELINUX_STATUS}" ]; then
        mkdir "${DEVICE}/${SELINUX_STATUS}"
    fi
    cp "${VBMETA}" .
    sudo chmod +777 *
    tar -cvf "LPoS ${LPOS_KERNEL_VERSION} [${DEVICE}] - ${SELINUX_STATUS}.tar" boot.img dt.img vbmeta.img ; rm boot.img dt.img vbmeta.img
    mv "LPoS ${LPOS_KERNEL_VERSION} [${DEVICE}] - ${SELINUX_STATUS}.tar" "${DEVICE}/${SELINUX_STATUS}"
}

tar_xz() {
    cd "$work_dir/out"
    tar -cvf "LPoS [${DEVICE}].tar" ./*
    xz -9 --threads=0 "LPoS [${DEVICE}].tar"
    mv "LPoS [${DEVICE}].tar.xz" "LPoS [${DEVICE}]-${current_datetime}.xz"
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

deep_clean(){
    cd "$work_dir"
    echo -e "[i] Cleaning Up...\n\n"
    make ${ARGS} clean && make ${ARGS} mrproper
}

lpos_defaults() {

    replace_config_option() {
        sed -i "s/^$1=.*/$1=$2/" "$config_file"
    }
    replace_config_option "CONFIG_KSU" "n"
    replace_config_option "CONFIG_SECURITY_SELINUX_ALWAYS_PERMISSIVE" "n"
}



clean_build() {
    cd "$work_dir"
    make ${ARGS} clean && make ${ARGS} mrproper
    lpos_defaults

    #patching allowlist for non-gki
    if [ ! -f ".allowlist_patched" ]; then
        patch -p1 < "$work_dir/ksu.patch"
        echo "1" > ".allowlist_patched"
    fi

    make ${ARGS} "$exynos_defconfig"
    make ${ARGS} -j"$(nproc)" || exit 1
    dtb_img
    mv "$work_dir/arch/arm64/boot/Image" "$dt_tool/AIK/split_img/boot.img-kernel"
    export SELINUX_STATUS="Enforcing"
    checks
    permissive
    tar_xz
}

dirty_build() {
    cd "$work_dir"
    make ${ARGS} "$exynos_defconfig"
    make ${ARGS} -j"$(nproc)" || exit 1
    dtb_img
    mv "$work_dir/arch/arm64/boot/Image" "$dt_tool/AIK/split_img/boot.img-kernel"
    checks
}

build_ksu(){
    #function for edit the config
    config_file="arch/arm64/configs/$exynos_defconfig"
    replace_config_option_ksu() {
    sed -i "s/^$1=.*/$1=$2/" "$config_file"
    }

    #ksu + enforcing
    ksu_enforce(){
        cd "$work_dir"
        replace_config_option_ksu "CONFIG_KSU" "y"
        replace_config_option_ksu "CONFIG_SECURITY_SELINUX_ALWAYS_PERMISSIVE" "n"
        export SELINUX_STATUS="Enforcing"
    }

    #building with ksu + enforce
    build_enforce() {
        cd "$work_dir"
        echo -e "\n\n[+] Compiling KernelSU + Enforcing..\n\n"
        make ${ARGS} "$exynos_defconfig"
        make ${ARGS} -j"$(nproc)" || exit 1
        dtb_img
        mv "$work_dir/arch/arm64/boot/Image" "$dt_tool/AIK/split_img/boot.img-kernel"
        checks_ksu
    }

    #ksu + Permissive
    ksu_permissive(){  
        cd "$work_dir"      
        replace_config_option_ksu "CONFIG_KSU" "y"
        replace_config_option_ksu "CONFIG_SECURITY_SELINUX_ALWAYS_PERMISSIVE" "y"
        export SELINUX_STATUS="Permissive"        
    }

    #building with ksu + permissive
    build_permissive() {
        cd "$work_dir"
        echo -e "\n\n[+] Compiling KernelSU + Permissive..\n\n"
        make ${ARGS} "$exynos_defconfig"
        make ${ARGS} -j"$(nproc)" || exit 1
        dtb_img
        mv "$work_dir/arch/arm64/boot/Image" "$dt_tool/AIK/split_img/boot.img-kernel"
        checks_ksu
    }

    # === Begin of core ===

    ksu_core(){

        dtb_img() {
            sudo chmod +777 $dt_tool/* -R
            $dt_tool/mkdtimg cfg_create "$work_dir/out/dt.img" "$dt_tool/exynos9820.cfg" -d "$work_dir/arch/arm64/boot/dts/exynos"
        }

        #check if the build succeed
        checks_ksu() {
        if [ -f "$dt_tool/AIK/split_img/boot.img-kernel" ]; then
            echo -e "\n\n[i] Task Finished ! \n"
            packing_ksu
        else
            echo -e "\n\n[i] Build Failed :( \n"
            exit 1
        fi
        }

        #packing process
        packing_ksu() {
            echo -e "\n\n[+] Repacking boot.img..."
            cd "$dt_tool/AIK/ramdisk"
            if [ ! -d "debug_ramdisk" ]; then
                mkdir -p debug_ramdisk dev metadata mnt proc second_stage_resources sys
            fi
            cd "$work_dir"
            sudo bash "$repacker"
            sudo chmod +777 "$dt_tool/AIK" -R
            echo -e "\n\n[+] Repacking Done..!"
            mv "$dt_tool/AIK/image-new.img" "$work_dir/out/boot.img"
            echo -e "\n\n[i] Creating a Flashable tar..!"

            cd "$work_dir/out"

            if [ ! -d "${DEVICE}-KSU" ]; then
                mkdir "${DEVICE}-KSU"
            fi
            if [ ! -d "${DEVICE}-KSU/${SELINUX_STATUS}" ]; then
                mkdir "${DEVICE}-KSU/${SELINUX_STATUS}"
            fi
            cp "${VBMETA}" .
            sudo chmod +777 *
            tar -cvf "LPoS ${LPOS_KERNEL_VERSION} [KSU] [${DEVICE}] - ${SELINUX_STATUS}.tar" boot.img dt.img vbmeta.img ; rm boot.img dt.img vbmeta.img
            mv "LPoS ${LPOS_KERNEL_VERSION} [KSU] [${DEVICE}] - ${SELINUX_STATUS}.tar" "${DEVICE}-KSU/${SELINUX_STATUS}"
            }

        #==== end of core ====
    }

    tar_xz_ksu() {
        cd "$work_dir/out"
        tar -cvf "LPoS [${DEVICE}][KSU].tar" "${DEVICE}-KSU"
        xz -9 --threads=0 "LPoS [${DEVICE}][KSU].tar"
        mv "LPoS [${DEVICE}][KSU].tar.xz" "KSU-LPoS [${DEVICE}]-${current_datetime}.xz"
        cd "$work_dir"
        echo -e "\n\n[i] KSU Compilation Done..🌛\n"
    }

    lets_build_kernelsu(){
        #compiling enforcing
        ksu_enforce
        ksu_core
        build_enforce
        #compiling permissive
        ksu_permissive
        ksu_core
        build_permissive
        #packing all
        tar_xz_ksu
    }
    lets_build_kernelsu
}

USER_INPUT=$1

if [ "$USER_INPUT" == "-c" ]; then
    echo -e "\n\n[i] Performing a clean build...\n\n"
    clean_build
elif [ "$USER_INPUT" == "-d" ]; then
    echo -e "\n\n[i] Performing a dirty build...\n\n"
    dirty_build
elif [ "$USER_INPUT" == "-x" ]; then
    echo -e "\n\n[i] Cleaning the source...\n\n"
    deep_clean 
elif [ "$USER_INPUT" == "-k" ]; then
    echo -e "\n\n[i] Building KernelSU...\n\n"
    build_ksu       
else
    echo -e "\n\n[x] Wrong Input..! \n\n [i] Usage : \n\n To a Clean build : build_kernel.sh -c\n To a dirty build : build_kernel.sh -d \n To Clean the source : build_kernel.sh -x\n To Build KernelSU : build_kernel.sh -k"
fi
