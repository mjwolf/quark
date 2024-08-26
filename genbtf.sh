#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024 Elastic NV

Script=${0##*/}

function usage
{
   echo "usage: $Script path-to-btfhub-archive" 1>&2
   exit 1
}

function die {
	printf "%s: %s\n" "$Script" "$1" 1>&2
	exit 1
}

if [ $# -ne 1 ]; then
   usage
fi

# amd64 only for now
Srcs="
amzn/2/x86_64
amzn/2018/x86_64
centos/7/x86_64
centos/8/x86_64
debian/bullseye/x86_64
debian/9/x86_64
debian/10/x86_64
fedora/24/x86_64
fedora/25/x86_64
fedora/26/x86_64
fedora/27/x86_64
fedora/28/x86_64
fedora/29/x86_64
ol/7/x86_64
ol/8/x86_64
rhel/7/x86_64
rhel/8/x86_64
sles/12.3
sles/12.5
sles/15.3
ubuntu/16.04/x86_64
ubuntu/18.04/x86_64
ubuntu/20.04/x86_64
"

cat <<EOF
// SPDX-License-Identifier: Apache-2.0
/* Copyright (c) 2024 Elastic NV */

#include "quark.h"

/*
 * THIS FILE IS AUTOGENERATED! Resist all urges to ruin its spirit manually!
 *
 * You can happily generate this through:
 * $ make btfhub BTFHUB_ARCHIVE_PATH=/my/path/to/a/btfhub-archive
 *
 * Grab a beer as this takes some time. It's 3000 files for amd64 only.
 */

EOF

Commit=$(cd $1 && git rev-parse HEAD)
if [ -z $Commit ]; then
	exit 1
fi

printf "const char *btfhub_archive_commit=\"%s\";\n\n" "$Commit"

# Table of all successfull kernels, so we can create all_btfs[]
typeset -a Good
typeset -i Total=0

for s in $Srcs; do
	distro=${s%/*}
	s="$1/$s"
	for k in $(find $s -name '*.tar.xz'); do
		btf=$(basename ${k%%.tar.xz})
		name="$distro-${btf%%.btf}"
		name=$(echo $name | tr \\055\\056\\057 _)
		tar xf $k || die "oh noes"
		if ./quark-btf -g $name -f $btf 2>/dev/null; then
			echo "$name OK" 1>&2
			Good+=("$name")
		else
			echo "$name FAIL" 1>&2
		fi
		rm -f $btf
		Total=$((Total+1))
	done
done

cat <<EOF
struct quark_btf *all_btfs[] = {
EOF

for name in "${Good[@]}"; do
	printf "\t&%s,\n" "$name"
done

cat <<EOF
	NULL
};

EOF

printf "%d/%d succeeded\n" ${#Good[@]} $Total 1>&2
