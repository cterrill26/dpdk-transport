#include <stdio.h>
#include "dpdk_transport.h"

int main(int argc, char *argv[]){
    init(argc, argv);

    long x = 7490;
    long y = 0;
    struct msginfo info;
    info.length = sizeof(x);
    int res = senddpdk(&x, &info);
    uint32_t len = recvdpdk(&y, &info);
    printf("%d, %u, %lu, %lu\n", res, len, x, y);

    x = 830234;
    res = senddpdk(&x, &info);
    len = recvdpdk(&y, &info);
    printf("%d, %u, %lu, %lu\n", res, len, x, y);
    return 0;
}