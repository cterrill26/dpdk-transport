#include <string>
#include <iostream>
#include <fstream>
#include <unistd.h>
#include <vector>
#include <unordered_set>
#include "dpdk_transport.h"

using namespace std;

enum MsgType
{
    START,
    DONE,
    REQUEST,
    RESPONSE
};

struct NodeAddr
{
    uint64_t mac;
    uint32_t ip;
};

vector<NodeAddr> get_addrs_from_file(const string &filename, NodeAddr &my_addr);
NodeAddr wait_for_start_msg();
void send_status_msg(const NodeAddr &src_addr, const NodeAddr &dst_addr, MsgType type);
void wait_for_done_msgs(const vector<NodeAddr> &addrs);
void main_loop(const NodeAddr &my_addr, const vector<NodeAddr> &other_addrs, int num_msgs, int msg_len);
void done_loop();

// lines in address file are assumed to be formated
// as "192.168.123.132,00:1b:63:84:45:e6" for ip and mac address,
// the first address will be the node's own addresses
vector<NodeAddr> get_addrs_from_file(const string &filename, NodeAddr &my_addr)
{
    ifstream f;
    vector<NodeAddr> addrs;
    bool first = true;

    f.open(filename, ios::in);
    if (f.is_open())
    {
        string ip, mac;
        while (getline(f, ip, ',') && getline(f, mac))
        {
            NodeAddr addr;
            addr.ip = string_to_ip(ip.c_str());
            addr.mac = string_to_mac(mac.c_str()) << 16;

            if (first)
            {
                my_addr = addr;
                first = false;
                continue;
            }

            addrs.push_back(addr);
        }

        f.close();
    }
    else
    {
        cerr << "Unable to open file: " << filename << endl;
        exit(1);
    }

    return addrs;
}

NodeAddr wait_for_start_msg()
{
    char buffer[MAX_MSG_SIZE];
    msg_info info;
    while (recv_dpdk((void *)buffer, &info, NULL) == 0)
        continue;

    if (buffer[0] != START)
    {
        cerr << "Node received incorrect starting message: " << buffer[0] << endl;
        exit(1);
    }

    NodeAddr ret;
    ret.ip = info.src_ip;
    ret.mac = info.src_mac;

    return ret;
}

void send_status_msg(const NodeAddr &src_addr, const NodeAddr &dst_addr, MsgType type)
{
    char buffer[] = {type};

    msg_info info;
    info.dst_ip = dst_addr.ip;
    info.dst_mac = dst_addr.mac;
    info.portid = 0;
    info.src_ip = src_addr.ip;
    info.src_mac = src_addr.mac;
    info.length = sizeof(buffer);

    while (send_dpdk(buffer, &info) < 0)
        continue;
}

void wait_for_done_msgs(const vector<NodeAddr> &addrs){
    unordered_set<uint32_t> ips;
    for (auto addr : addrs){
        ips.insert(addr.ip);
    }

    char buffer[MAX_MSG_SIZE];
    msg_info info;

    while(!ips.empty()){
        while (recv_dpdk((void *)buffer, &info, NULL) == 0)
            continue;

        if (buffer[0] != DONE)
        {
            cerr << "Controller node received incorrect done message: " << buffer[0] << endl;
            exit(1);
        }

        ips.erase(info.src_ip);
    }
}

void main_loop(const NodeAddr &my_addr, const vector<NodeAddr> &other_addrs, int num_msgs, int msg_len)
{
    char buffer[MAX_MSG_SIZE];
    for (int i = 0; i < num_msgs; i++)
    {
        // first send a request message
        NodeAddr dst_addr = other_addrs[i % other_addrs.size()];

        msg_info info;
        info.length = msg_len;
        info.src_ip = my_addr.ip;
        info.src_mac = my_addr.mac;
        info.portid = 0;
        info.dst_ip = dst_addr.ip;
        info.dst_mac = dst_addr.mac;

        buffer[0] = REQUEST;
        for (int b = 1; b < msg_len; b++)
            buffer[b] = (i + b) % 256;

        while (send_dpdk(buffer, &info) < 0)
            continue;

        // next wait for response, and respond to other requests
        while (true)
        {
            while (recv_dpdk(buffer, &info, NULL) == 0)
                continue;

            if (buffer[0] == RESPONSE)
            {
                // receive echo response, check msg length
                if (info.length != ((uint32_t) msg_len))
                {
                    cerr << "incorrect response msg length: " << info.length << " expected: " << msg_len << endl;
                    exit(1);
                }

                // verify each byte is correct
                for (int b = 1; b < msg_len; b++)
                    if (((unsigned int) buffer[b]) != ((unsigned int)((i + b) % 256)))
                    {
                        cerr << "incorrect response msg content: " << ((unsigned int) buffer[b]) << " expected: " << ((unsigned int)((i + b) % 256)) << " index: " << b << endl;
                        exit(1);
                    }

                // move on to next request to send
                break;
            }
            else if (buffer[0] == REQUEST)
            {
                // send a echo response to a request
                swap(info.dst_ip, info.src_ip);
                swap(info.dst_mac, info.src_mac);
                buffer[0] = RESPONSE;

                while (send_dpdk(buffer, &info) < 0)
                    continue;
            }
        }
    }
}

void done_loop()
{
    char buffer[MAX_MSG_SIZE];
    msg_info info;

    while (true)
    {
        while (recv_dpdk(buffer, &info, NULL) == 0)
            continue;

        if (buffer[0] == REQUEST)
        {
            // send a echo response to a request
            swap(info.dst_ip, info.src_ip);
            swap(info.dst_mac, info.src_mac);
            buffer[0] = RESPONSE;

            while (send_dpdk(buffer, &info) < 0)
                continue;
        }
        else if (buffer[0] == DONE)
        {
            break;
        }
    }
}

int main(int argc, char *argv[])
{
    int ret = init_dpdk(argc, argv, F_SINGLE_SEND | F_SINGLE_RECV);
    argc -= ret;
    argv += ret;

    bool f_flag = false;
    string filename;
    bool is_controller = false;
    int num_msgs = 1000;
    int msg_len = 1000;

    char c;
    while ((c = getopt(argc, argv, "f:cn:l")) != -1)
        switch (c)
        {
        case 'f':
            f_flag = true;
            filename = optarg;
            break;
        case 'c':
            is_controller = true;
            break;
        case 'n':
            num_msgs = atoi(optarg);
            break;
        case 'l':
            msg_len = atoi(optarg);
            break;
        }

    if (!f_flag)
    {
        cerr << "missing -f for MAC and IP addresses file" << endl;
        exit(1);
    }

    if (num_msgs <= 0)
    {
        cerr << "number of messages must be positive: " << num_msgs << endl;
        exit(1);
    }

    if (msg_len <= 0)
    {
        cerr << "msg length must be positive: " << msg_len << endl;
        exit(1);
    }

    NodeAddr my_addr;
    auto other_addrs = get_addrs_from_file(filename, my_addr);

    if (is_controller)
    {
        // this nodes simply starts the other nodes and collects
        // performance measurements at the end

        cout << "starting controller" << endl;
        cout << "sending start msgs" << endl;
        for (auto addr : other_addrs)
        {
            send_status_msg(my_addr, addr, START);
        }

        cout << "waiting for done messages" << endl;
        wait_for_done_msgs(other_addrs);

        cout << "sending done messages" << endl;
        // send done message to nodes to tell them to terminate
        for (auto addr : other_addrs)
        {
            send_status_msg(my_addr, addr, DONE);
        }

        return 0;
    }

    cout << "starting worker nodes" << endl;

    cout << "waiting for start msg" << endl;
    // this node is not the controller, so it must wait for the
    // controller's start message
    auto controller_addr = wait_for_start_msg();

    sleep(2);

    cout << "starting main loop" << endl;
    main_loop(my_addr, other_addrs, num_msgs, msg_len);

    cout << "sending done msg" << endl;
    send_status_msg(my_addr, controller_addr, DONE);

    cout << "starting done loop" << endl;
    done_loop();

    cout << "terminating" << endl;
    sleep(2);

    terminate_dpdk();

    return 0;
}