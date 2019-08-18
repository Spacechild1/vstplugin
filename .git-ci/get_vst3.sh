#!/bin/sh

VST3DIR=$1
if [ "x$1" = "x" ]; then
 VST3DIR=vst/VST_SDK/VST3_SDK/
fi

rm -rf "${VST3DIR}"
git clone https://github.com/steinbergmedia/vst3_base "${VST3DIR}/base/"
git clone https://github.com/steinbergmedia/vst3_pluginterfaces "${VST3DIR}/pluginterfaces/"
git clone https://github.com/steinbergmedia/vst3_public_sdk "${VST3DIR}/public.sdk/"
