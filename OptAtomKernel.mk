############################################################################
############# KERNEL OPTIMIZATION FLAGS FOR THE ATOM Z2480 #################
############################################################################
#############                By Oxavelar                   #################
############################################################################

Z2480_OPTIMIZATION_FLAGS := \
        -march=core2 \
        -mtune=pentium3 \
        -mx32 \
        -mssse3 \
        -mcx16 \
        -msahf \
        -mmovbe \
        -fomit-frame-pointer \
        --param l1-cache-line-size=64 \
        --param l1-cache-size=24 \
        --param l2-cache-size=512 \

ANDROID_TOOLCHAIN_FLAGS := \
        -mno-android \
        -O2 \
        -ftree-vectorize \
        -floop-block \
        -floop-interchange \
        -floop-strip-mine \
        -fgraphite-identity \
        -ftree-loop-distribution \
        -ftree-loop-linear \
        -ftree-loop-im \
        -ftree-loop-if-convert \
        -ftree-loop-if-convert-stores \
        -foptimize-register-move \
        -fmodulo-sched \
        -fmodulo-sched-allow-regmoves \
        $(Z2480_OPTIMIZATION_FLAGS) \

BUILTIN_TOOLCHAIN_FLAGS := \
        -pipe \
        -flto \
        -floop-parallelize-all \
        -ftree-parallelize-loops=2 \

# Extra CFlags for specific (builtin) modules
# The following modules have problems with -ftree-vectorize
# and if removed will get battery reading errors
export CFLAGS_platform_max17042.o       := -fno-tree-vectorize
export CFLAGS_max17042_battery.o        := -fno-tree-vectorize
export CFLAGS_intel_mdf_battery.o       := -fno-tree-vectorize
