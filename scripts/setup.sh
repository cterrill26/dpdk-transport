export RTE_SDK=/home/ec2-user/dpdk-stable-19.11.14
export RTE_TARGET=x86_64-native-linuxapp-gcc
export DPDK_IPV4=$(ifconfig eth1 | grep 'inet ' | awk '{print $2}') 
ifconfig eth1 down
$RTE_SDK/usertools/dpdk-devbind.py --bind=igb_uio eth1
sleep 1