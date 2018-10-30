#!/bin/sh

SDKURL="https://download.steinberg.net/sdk_downloads/vstsdk3611_22_10_2018_build_34.zip"

curl -s -o vstsdk.zip "${SDKURL}"

unzip vstsdk.zip -d src

