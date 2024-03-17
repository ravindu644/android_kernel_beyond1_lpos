#clone proton clang v12 toolchain
mkdir -p toolchain
wget https://github.com/kdrag0n/proton-clang/archive/refs/tags/20210123.zip
unzip 20210123.zip
mv proton-clang-20210123 proton-clang
mv proton-clang toolchain