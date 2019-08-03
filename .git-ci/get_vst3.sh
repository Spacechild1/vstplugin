#!/bin/sh

VST3DIR=$1
if [ "x$1" = "x" ]; then
 VST3DIR=vst/VST_SDK/VST3_SDK/
fi

mkdir -p "${VST3DIR}/pluginterfaces/"
rm -rf "${VST3DIR}/pluginterfaces/"
git clone https://github.com/steinbergmedia/vst3_pluginterfaces "${VST3DIR}/pluginterfaces/"
