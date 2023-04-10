id
source scripts/setup.sh
make
cd tests/initiator/
find ./*/app/initiator -exec '{}'  '--' "-s $DPDK_IPV4" '-d 172.31.6.161' '-m 0a:0a:fb:9e:c1:f1' ';'
