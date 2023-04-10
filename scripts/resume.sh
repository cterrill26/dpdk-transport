export RTE_SDK=/home/ec2-user/dpdk-stable-19.11.14
export RTE_TARGET=x86_64-native-linuxapp-gcc
DEVICE=$($RTE_SDK/usertools/dpdk-devbind.py --status | grep drv=igb_uio | awk '{print $1}')
$RTE_SDK/usertools/dpdk-devbind.py -u $DEVICE
$RTE_SDK/usertools/dpdk-devbind.py --bind=ena $DEVICE
sleep 1
export DPDK_IPV4=$(ifconfig eth1 | grep 'inet ' | awk '{print $2}') 
ifconfig eth1 down
$RTE_SDK/usertools/dpdk-devbind.py --bind=igb_uio eth1
sleep 1