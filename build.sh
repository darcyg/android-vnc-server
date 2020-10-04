#!/bin/bash

rm -rf ./obj
ndk-build APP_PLATFORM=android-23 PLATFORM_SDK_VERSION=23
