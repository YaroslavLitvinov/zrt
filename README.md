# About

ZRT - ZeroVM Run-Time environment for user executables running on
virtual hardware represented by ZeroVM. ZRT act as part of glibc
library and implements zerovm platform dependent functions. ZVM
toolchain must be used in order to create user applications;

# Install & Run

1. Install ZeroVM, Consult
   https://github.com/zerovm/zerovm/blob/master/README.md

2. Install ZVM SDK. Consult https://github.com/zerovm/toolchain/blob/master/README.md

3. Install zrt

        git clone https://github.com/zerovm/zrt.git

4. Set environmant variables on ~/.bashrc

        ZVM_PREFIX    -path to zvm toolchain
        ZEROVM_ROOT   -path to ZEROVM folder, used for debugging with gdb
        ZRT_ROOT      -path to ZRT
        ZPYTHON_ROOT  -path to zpython port from cpython2 / cpython3

    For example:

        export ZEROVM_ROOT=${HOME}/zerovm
        export ZRT_ROOT=${HOME}/zrt
        export ZPYTHON_ROOT=${HOME}/zpython

5. run

        cd ${ZRT_ROOT}
        make
