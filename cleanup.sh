#!/bin/bash

dir=/data/local/tmp

set -exo pipefail

adb uninstall jp.co.cyberagent.stf.rotationwatcher
adb shell rm -f $dir/androidvncserver $dir/rotate.apk $dir/minicap.so $dir/droidvncserver-run.sh

set -o pipefail

echo "Done."

