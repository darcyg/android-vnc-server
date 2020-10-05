#!/bin/bash

dir=/data/local/tmp

abi=$(adb shell getprop ro.product.cpu.abi | tr -d '\r')
sdk=$(adb shell getprop ro.build.version.sdk | tr -d '\r')
rel=$(adb shell getprop ro.build.version.release | tr -d '\r')

#abi=armeabi-v7a
#sdk=29
#rel=29

send_minicap=$(adb shell "ls $dir/minicap.so &>/dev/null; echo -n \$?")
send_rotate=$(adb shell "ls $dir/rotate.apk &>/dev/null; echo -n \$?")

echo "Directory: $dir"
echo "Compiling androidvncserver.."
#ndk-build
if [ $? -ne 0 ]; then
    echo "Compilation failed."
    exit 1
fi

echo "Sending androidvncserver.."
adb push libs/$abi/androidvncserver $dir/

if [ $send_rotate -eq "1" ]; then
    echo "Sending RotationWatcher"
    adb push rotate.apk $dir
else
    echo "Skipping RotationWatcher"
fi

if [ $send_minicap -eq "1" ]; then
    echo "Sending minicap"
    if [ -e jni/minicap-shared/aosp/libs/android-$rel/$abi/minicap.so ]; then
        adb push jni/minicap-shared/aosp/libs/android-$rel/$abi/minicap.so $dir/
    else
        adb push jni/minicap-shared/aosp/libs/android-$sdk/$abi/minicap.so $dir/
    fi
else
    echo "Skipping minicap"
fi

echo "Starting androidvncserver.."
echo "----------------------------------------"
adb push ./droidvncserver-run.sh $dir/
adb shell chmod 777 $dir/droidvncserver-run.sh
#adb shell "sh $dir/droidvncsever-run.sh"
#exec adb shell LD_LIBRARY_PATH=$dir exec $dir/droidvncsever-run.sh $@
# adb shell
# su
# LD_LIBRARY_PATH=$dir $dir/androidvncserver
