#!/bin/sh

VST2DIR=$1
if [ "x$1" = "x" ]; then
 VST2DIR=vst/VST_SDK/VST2_SDK/
fi
VST2REPO=https://raw.githubusercontent.com/WeAreROLI/JUCE/a54535bc317b5c005a8cda5c22a9c8d4c6e0c67e/modules/juce_audio_processors/format_types/VST3_SDK/pluginterfaces/vst2.x


mkdir -p "${VST2DIR}/pluginterfaces/vst2.x/"
for f in aeffect.h  aeffectx.h  vstfxstore.h; do
  echo "fetching $f" 1>&2
  curl -s -o "${VST2DIR}/pluginterfaces/vst2.x/${f}" "${VST2REPO}/${f}"
done
