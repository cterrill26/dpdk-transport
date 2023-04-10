source scripts/setup.sh
make
./tests/initiator/$RTE_TARGET/app/initiator -- -s $DPDK_IPV4 -d 172.31.6.161 -m 0a:0a:fb:9e:c1:f1 