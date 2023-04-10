cd /home/ec2-user/dpdk-stable-19.11.14
export RTE_SDK=`pwd`
export RTE_TARGET=x86_64-native-linuxapp-gcc
DEVICE=$(./usertools/dpdk-devbind.py --status | grep drv=igb_uio | awk '{print $1}')
./usertools/dpdk-devbind.py -u $DEVICE
./usertools/dpdk-devbind.py --bind=ena $DEVICE
sleep 1
export DPDK_IPV4=$(ifconfig eth1 | grep 'inet ' | awk '{print $2}') 
ifconfig eth1 down
./usertools/dpdk-devbind.py --bind=igb_uio eth1
sleep 1
cd $OLDPWD
