cd /home/ec2-user/dpdk-stable-19.11.14
export RTE_SDK=`pwd`
export RTE_TARGET=x86_64-native-linuxapp-gcc
echo 1024 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
echo 0 > /proc/sys/kernel/randomize_va_space
modprobe uio
modprobe hwmon
insmod ./x86_64-native-linuxapp-gcc/kmod/rte_kni.ko
insmod ./x86_64-native-linuxapp-gcc/kmod/igb_uio.ko
export DPDK_IPV4=$(ifconfig eth0 | grep 'inet ' | awk '{print $2}') 
ifconfig eth0 down
./usertools/dpdk-devbind.py --bind=igb_uio eth0
cd $OLDPWD
