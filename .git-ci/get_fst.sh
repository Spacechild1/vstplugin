#!/bin/sh

VST2DIR=$1
if [ "x$1" = "x" ]; then
 VST2DIR=vst/VST_SDK/VST2_SDK/
fi

FSTREPO=https://git.iem.at/zmoelnig/FST.git

mkdir -p "${VST2DIR}"
rm -rf "${VST2DIR}"
git clone https://git.iem.at/zmoelnig/FST.git "${VST2DIR}/fst"
mkdir -p "${VST2DIR}/pluginterfaces/"
(cd "${VST2DIR}/fst"; ln -s "../fst/fst/" "../pluginterfaces/vst2.x")

