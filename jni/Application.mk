#NDK_TOOLCHAIN_VERSION=4.9
APP_CPPFLAGS += -std=c++11 -fexceptions
APP_STL := c++_static

APP_CFLAGS += \
	-Ofast \
	-funroll-loops \
	-fno-strict-aliasing

# APP_LDFLAGS += -lm_hard

APP_CFLAGS += \
#	-march=armv7-a \
	-mfpu=neon \
	-mfloat-abi=softfp \
#	-marm \
	-fprefetch-loop-arrays \
	-DHAVE_NEON=1

#APP_ABI:=arm64-v8a
APP_ABI:=x86
APP_OPTIM := debug
APP_PLATFORM := android-23
#APP_ALLOW_MISSING_DEPS :=true
#APP_MODULES:= jpeg libpng libcrypto_static libssl_static androidvncserver
