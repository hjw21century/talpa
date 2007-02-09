#! /bin/bash
#
# TALPA test script
#
# Copyright (C) 2004 Sophos Plc, Oxford, England.
#
# This program is free software; you can redistribute it and/or modify it under the terms of the
# GNU General Public License Version 2 as published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
# even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
# See the GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along with this program; if not,
# write to the Free Software Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
#

${srcdir}/talpa-init.sh

tmpdir='/tmp/tlp-test'

mkdir -p ${tmpdir}/mnt1
mkdir -p ${tmpdir}/mnt2

dd if=/dev/zero of=${tmpdir}/fs1.img bs=1M count=4 >/dev/null 2>&1
dd if=/dev/zero of=${tmpdir}/fs2.img bs=1M count=4 >/dev/null 2>&1

mkfs='/sbin/mkfs.minix'
fs=minix
if [ ! -x "$mkfs" ]; then
    mkfs='/sbin/mkfs.vfat'
    fs=vfat
    if [ ! -x "$mkfs" ]; then
        mkfs=''
    fi
fi

if [ "$mkfs" = "" ]; then
    exit 77
fi

${mkfs} ${tmpdir}/fs1.img >/dev/null
${mkfs} ${tmpdir}/fs2.img >/dev/null

mount -t $fs ${tmpdir}/fs1.img ${tmpdir}/mnt1 -o loop
mount -t $fs ${tmpdir}/fs2.img ${tmpdir}/mnt2 -o loop
umount ${tmpdir}/mnt1
umount ${tmpdir}/mnt2

# Ignore the results because we only care about crashing or surviving here
exit 0