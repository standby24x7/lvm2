#!/bin/sh
# Copyright (C) 2015 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

# check if 'dmsetup --noflush' will work properly for mounted snapshot

. lib/inittest

which mkfs.ext2 || skip

test -e LOCAL_LVMETAD && skip
test -e LOCAL_LVMPOLLD && skip
test -e LOCAL_CLVMD && skip

aux prepare_vg 5

# Create stacked device
lvcreate --type snapshot -s -L10 -n $lv1 $vg --virtualsize 100M
aux extend_filter_LVMTEST
vgcreate $vg1 "$DM_DEV_DIR"/$vg/$lv1


lvcreate -L20 -n $lv1 $vg1
lvcreate -L10 -n snap -s $vg1/$lv1

mkfs.ext2 "$DM_DEV_DIR/$vg1/snap"
mkdir mnt
mount -o errors=remount-ro "$DM_DEV_DIR/$vg1/snap" mnt

sync

# intetionally suspend layer bellow
dmsetup suspend $vg-$lv1

# now this should pass without blocking
#dmsetup suspend --noflush --nolockfs $vg1-snap &
dmsetup suspend $vg1-snap &

sleep .5

dmsetup info --noheadings -c -o suspended $vg1-snap | tee out
should grep -i suspend out

# unlock device below
dmsetup resume $vg-$lv1
# so this will pass without blocking on udev
# otherwise --noudevsync would be needed
dmsetup resume $vg1-snap


# Try how force removal works
dmsetup suspend $vg-$lv1
# needs to fail as device is still open
not dmsetup remove --force $vg1-snap
dmsetup resume $vg-$lv1

# check it really is now 'error' target
dmsetup table $vg1-snap | tee out
grep error out

umount mnt || true

lvremove -f $vg1

vgremove -ff $vg1
vgremove -ff $vg
