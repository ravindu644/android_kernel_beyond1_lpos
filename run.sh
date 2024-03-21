#!/bin/bash
read -p "Enter Kernel Version : " value
export LPOS_KERNEL_VERSION=${value}

if [ -f ".kernelsu-fetch-lock" ]; then
    rm -rf out ; rm .allowlist_patched ; rm .kernelsu-fetch-lock ; rm -rf drivers/kernelsu
fi
bash build_kernel.sh -c

KSU(){
    read -p "Do you want to compiler KernelSU..? (y/n) : " INPUT
    if [ "$INPUT" == "y" ]; then
        bash build_kernel.sh -k
    if [ "$INPUT"="n" ]; then
        exit
    else
        echo -e "\n[x] Wrong Input..!\n"
        KSU
    fi
}

KSU