#!/bin/bash
#
# Copyright (C) 2016 The CyanogenMod Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

set -e

DEVICE=CP8298_I00
VENDOR=coolpad

# Load extractutils and do some sanity checks
MY_DIR="${BASH_SOURCE%/*}"
if [[ ! -d "$MY_DIR" ]]; then MY_DIR="$PWD"; fi

LINEAGE_ROOT="$MY_DIR"/../../..

HELPER="$LINEAGE_ROOT"/vendor/lineage/build/tools/extract_utils.sh
if [ ! -f "$HELPER" ]; then
    echo "Unable to find helper script at $HELPER"
    exit 1
fi
. "$HELPER"

# Initialize the helper
setup_vendor "$DEVICE" "$VENDOR" "$LINEAGE_ROOT"

# Sections ending in " - userdebug" or " - GMS" are only installed on those builds
function section_list() {
    awk -v tag="$1" '/^# /{ t = ""; if (match($0, / - (userdebug|GMS)$/)) t = substr($0, RSTART + 3) } t == tag' \
        "$MY_DIR"/proprietary-files.txt > "$TMPDIR/$2"
}
section_list "" main.txt
section_list userdebug userdebug.txt
section_list GMS gms.txt

# Copyright headers and guards
write_headers

write_makefiles "$TMPDIR"/main.txt

printf '\n%s\n' "ifneq (\$(TARGET_BUILD_VARIANT),user)" >> "$PRODUCTMK"
write_makefiles "$TMPDIR"/userdebug.txt
printf '%s\n' "endif" >> "$PRODUCTMK"

printf '\n%s\n' "ifeq (\$(WITH_GMS),true)" >> "$PRODUCTMK"
write_makefiles "$TMPDIR"/gms.txt
printf '%s\n' "endif" >> "$PRODUCTMK"

# We are done!
write_footers
