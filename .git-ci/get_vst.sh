#!/bin/sh
OUTFILE=vstsdk2.zip
SDKURL="https://download.steinberg.net/sdk_downloads/vstsdk3611_22_10_2018_build_34.zip"

curl -s -o "${OUTFILE}" "${SDKURL}"

unzip -q "${OUTFILE}" -d src
rm -f "${OUTFILE}"
