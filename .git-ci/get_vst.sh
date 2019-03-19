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
  # the sslcainfo thing is a hack to allow a 'git clone' with a borked git
  # - mingw32/git: 2.19.0 (in /usr/bin/)
  # - mingw64/git: 2.19.1.windows.1 (in /mingw64/bin/)
  # gitlab-runner set the 'http.https://git.iem.at.sslcainfo' configuration key
  #  to some file ("/c/Users/...")
  # mingw64/git prefixes the filename with an additional "C:/msys64/mingw64"
  #  but then fails to find the file and cannot clone.
  sslcainfo=$(git config --get http.https://git.iem.at.sslcainfo)
  git config --global --unset http.https://git.iem.at.sslcainfo
  git clone https://git.iem.at/zmoelnig/FST.git "${VST2DIR}"
  if [ "x${sslcainfo}" != "x" ]; then
    git config --global --add http.https://git.iem.at.sslcainfo "${sslcainfo}"
  fi
  mkdir -p "${VST2DIR}/pluginterfaces/"
  (cd "${VST2DIR}/pluginterfaces/"; ln -s ../fst vst2.x)
}

roli2vst
