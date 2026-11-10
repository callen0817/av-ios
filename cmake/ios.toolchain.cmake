# CMake Toolchain File for Cross-Compiling to iOS ARM64 from Windows (TheosWin / LLVM) or macOS
set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_SYSTEM_VERSION 16.0)
set(CMAKE_SYSTEM_PROCESSOR arm64)

set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Target iOS architecture")
set(CMAKE_IOS_DEPLOYMENT_TARGET "16.0" CACHE STRING "Minimum iOS deployment version")

# Detect iOS SDK path from environment (THEOS / IOS_SDK_ROOT)
if(DEFINED ENV{IOS_SDK_ROOT} AND EXISTS "$ENV{IOS_SDK_ROOT}")
    set(AVSCAN_IOS_SDK_ROOT "$ENV{IOS_SDK_ROOT}")
elseif(DEFINED ENV{THEOS} AND EXISTS "$ENV{THEOS}/sdks")
    file(GLOB THEOS_SDKS "$ENV{THEOS}/sdks/iPhoneOS*.sdk")
    list(LENGTH THEOS_SDKS NUM_SDKS)
    if(NUM_SDKS GREATER 0)
        list(GET THEOS_SDKS -1 AVSCAN_IOS_SDK_ROOT)
    endif()
endif()

if(DEFINED AVSCAN_IOS_SDK_ROOT AND EXISTS "${AVSCAN_IOS_SDK_ROOT}")
    set(CMAKE_OSX_SYSROOT "${AVSCAN_IOS_SDK_ROOT}" CACHE PATH "iOS SDK Sysroot" FORCE)
else()
    message(STATUS "AVScan iOS Toolchain: No local iPhoneOS.sdk found in IOS_SDK_ROOT or THEOS/sdks.")
endif()

# Locate Clang / Clang++ capable of targeting arm64-apple-ios16.0
if(WIN32)
    find_program(IOS_CLANG_COMPILER NAMES clang clang.exe
        HINTS
            "$ENV{THEOS}/toolchain/windows/iphone/bin"
            "$ENV{LLVM_ROOT}/bin"
            "C:/Program Files/LLVM/bin"
    )
    find_program(IOS_CLANGXX_COMPILER NAMES clang++ clang++.exe
        HINTS
            "$ENV{THEOS}/toolchain/windows/iphone/bin"
            "$ENV{LLVM_ROOT}/bin"
            "C:/Program Files/LLVM/bin"
    )
    find_program(IOS_LLD_LINKER NAMES ld64.lld ld64.lld.exe lld lld.exe
        HINTS
            "$ENV{THEOS}/toolchain/windows/iphone/bin"
            "$ENV{LLVM_ROOT}/bin"
            "C:/Program Files/LLVM/bin"
    )

    if(IOS_CLANG_COMPILER AND IOS_CLANGXX_COMPILER)
        set(CMAKE_C_COMPILER "${IOS_CLANG_COMPILER}")
        set(CMAKE_CXX_COMPILER "${IOS_CLANGXX_COMPILER}")
        set(CMAKE_OBJCXX_COMPILER "${IOS_CLANGXX_COMPILER}")
    endif()
endif()

set(APPLE_TARGET_TRIPLE "arm64-apple-ios16.0")
set(CMAKE_C_COMPILER_TARGET "${APPLE_TARGET_TRIPLE}")
set(CMAKE_CXX_COMPILER_TARGET "${APPLE_TARGET_TRIPLE}")
set(CMAKE_OBJCXX_COMPILER_TARGET "${APPLE_TARGET_TRIPLE}")

set(COMMON_IOS_FLAGS "-target ${APPLE_TARGET_TRIPLE} -miphoneos-version-min=16.0 -fPIC")
if(DEFINED AVSCAN_IOS_SDK_ROOT)
    set(COMMON_IOS_FLAGS "${COMMON_IOS_FLAGS} -isysroot \"${AVSCAN_IOS_SDK_ROOT}\" -F\"${AVSCAN_IOS_SDK_ROOT}/System/Library/Frameworks\"")
endif()

set(CMAKE_C_FLAGS_INIT "${COMMON_IOS_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${COMMON_IOS_FLAGS} -stdlib=libc++")
set(CMAKE_OBJCXX_FLAGS_INIT "${COMMON_IOS_FLAGS} -stdlib=libc++ -fobjc-arc")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
