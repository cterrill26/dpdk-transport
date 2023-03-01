#include <stdio.h>
#include "dpdk_transport.h"

int main(int argc, char *argv[]){
    
    init(argc, argv);

    long x = 7490;
    long y = 0;
    struct msginfo info;
    info.length = sizeof(x);
    int res = send_dpdk(&x, &info);
    uint32_t len = recv_dpdk(&y, &info);
    printf("%d, %u, %lu, %lu\n", res, len, x, y);

    x = 830234;
    res = send_dpdk(&x, &info);
    len = recv_dpdk(&y, &info);
    printf("%d, %u, %lu, %lu\n", res, len, x, y);

    terminate();

    return 0;
}