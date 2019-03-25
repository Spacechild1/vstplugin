#!/bin/sh

VST2DIR=$1
if [ "x$1" = "x" ]; then
 VST2DIR=vst/VST_SDK/VST2_SDK/
fi

FSTREPO=https://git.iem.at/zmoelnig/FST.git
VST2ROLI=https://raw.githubusercontent.com/WeAreROLI/JUCE/a54535bc317b5c005a8cda5c22a9c8d4c6e0c67e/modules/juce_audio_processors/format_types/VST3_SDK/pluginterfaces/vst2.x


roli2vst() {
mkdir -p "${VST2DIR}/pluginterfaces/vst2.x/"
for f in aeffect.h  aeffectx.h  vstfxstore.h; do
  echo "fetching $f" 1>&2
  curl -s -o "${VST2DIR}/pluginterfaces/vst2.x/${f}" "${VST2ROLI}/${f}"
done
}

fst2vst() {
  mkdir -p "${VST2DIR}"
  rm -rf "${VST2DIR}"
  git clone https://git.iem.at/zmoelnig/FST.git "${VST2DIR}"
  mkdir -p "${VST2DIR}/pluginterfaces/"
  (cd "${VST2DIR}/pluginterfaces/"; ln -s ../fst vst2.x)
}

roli2vst
