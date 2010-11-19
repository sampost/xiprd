#!/bin/bash
#
# a simple test to load & exercise the driver
#

dev=xiprd0
ko=xiprd
mount=/mnt/xip

# bash function to write & read-verify a sector using dd.
# definitely not the most efficient method, just meant
# to find any holes even if it's slow
function read_write_sector()
{
   let mod=$sector%1000
   if [ "$mod" == "0" ]; then
      echo "rw $sector"
   fi

   sudo dd if=foo.bin of=/dev/xiprd0 bs=512 count=1 seek=$sector > /dev/null 2>&1
   sudo dd of=foo2.bin if=/dev/xiprd0 bs=512 count=1 skip=$sector > /dev/null 2>&1
   diff foo.bin foo2.bin > /dev/null
   if [ "$?" != "0" ]; then
      echo "Mismatch on $sector"
   fi
}


##################################
# MAIN LOGIC

echo "======= Loading the driver =========="
sudo dmesg -c > /dev/null
sudo insmod ./$ko.ko
sudo dmesg -c

# decide which sector to start WRV test on
startsect=0
if [ "$1" != "fio" ]; then
   startsect=$1
fi
echo "startsect=$startsect"

# ending sector for WRV test
sectors=`cat /sys/block/$dev/size`
endsect=`echo $[$sectors-1]`
if [ "$2" != "" ]; then
   endsect=$2
fi
echo "endsect=$endsect"


echo "======= Generating Additional IO =========="
if [ "$1" == "fio" ]; then
   sudo fio --bs=4k --ioengine=libaio --iodepth=4 --numjobs=5 --size=1g   \
	   --direct=1 --runtime=10 --filename=/dev/$dev --name=seq-read \
	   --rw=rwmix --time_based --group_reporting
elif [ "$1" == "mount" ]; then
   sudo mkfs.ext4 /dev/$dev
   sudo mount -t ext4 /dev/$dev $mount && sudo cp foo.bin $mount/ && diff $foo.bin $mount/foo.bin
   sudo umount $mount
else
   # exercise every sector in a range
   for sector in `seq $startsect $endsect`; do
      read_write_sector
   done
fi

echo "======= Unloading driver =========="
sudo rmmod $ko
sudo dmesg -c
