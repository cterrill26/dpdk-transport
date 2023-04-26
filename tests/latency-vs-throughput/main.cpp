#include <string>
#include <cstring>
#include <iostream>
#include <fstream>
#include <unistd.h>
#include <vector>
#include <unordered_set>
#include <chrono>
#include <random>
#include <thread>
#include "dpdk_transport.h"

using namespace std;

enum MsgType
{
    START,
    DONE,
    REQUEST,
    RESPONSE,
    TERMINATE
};

struct NodeAddr
{
    uint64_t mac;
    uint32_t ip;
};

vector<NodeAddr> get_addrs_from_file(const string &filename, NodeAddr &my_addr);
void wait_for_msg(NodeAddr &addr, MsgType &type, string &content);
void send_msg(const NodeAddr &src_addr, const NodeAddr &dst_addr, MsgType type, const string &body, double mean);
void wait_for_done_msgs(const vector<NodeAddr> &addrs);
void send_thread(const NodeAddr &my_addr, const vector<NodeAddr> &other_addrs, int num_msgs, int msg_len, double mean);
unsigned long long worker_loop(const NodeAddr &my_addr, const vector<NodeAddr> &other_addrs, int num_msgs, int msg_len);
void done_loop();
void controller_loop(const NodeAddr &my_addr, const vector<NodeAddr> &other_addrs, double mean_start, double mean_end, double mean_increment);

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

void wait_for_msg(NodeAddr &addr, MsgType &type, string &content)
{
    char buffer[MAX_MSG_SIZE];
    msg_info info;

    while (recv_dpdk((void *)buffer, &info, NULL) == 0)
        continue;

    addr.ip = info.src_ip;
    addr.mac = info.src_mac;
    type = ((MsgType) buffer[0]);
    content = &buffer[1];
}

void send_msg(const NodeAddr &src_addr, const NodeAddr &dst_addr, MsgType type, const string &content)
{
    char buffer[content.length() + 2];
    buffer[0] = type;
    strcpy(&buffer[1], content.c_str());

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

    unsigned char buffer[MAX_MSG_SIZE];
    msg_info info;
    long long int total_latency = 0;

    while(!ips.empty()){
        while (recv_dpdk((void *)buffer, &info, NULL) == 0)
            continue;

        if (buffer[0] != DONE)
        {
            cerr << "Controller node received incorrect done message: " << buffer[0] << endl;
            exit(1);
        }
        else if (info.length <= 1){
            cerr << "Controller node received empty done message body" << endl;
            exit(1);
        }

        string latency = (char*) &buffer[1];
        cout << "Single node's latency: " << latency << endl;
        total_latency += stoll(latency);
        ips.erase(info.src_ip);
    }

    cout << "Avg node latency: " << total_latency / addrs.size() << endl;
}

void send_thread(const NodeAddr &my_addr, const vector<NodeAddr> &other_addrs, int num_msgs, int msg_len, double mean){
    char buffer[MAX_MSG_SIZE];
    default_random_engine generator(0);
    exponential_distribution<float>distribution(mean);
    auto next_send_time = chrono::high_resolution_clock::now();

    for (int i = 0; i < num_msgs; i++){
        chrono::nanoseconds delay((long long int) round(distribution(generator)));
        next_send_time += delay;
        auto next_send_time_ns = chrono::duration_cast<chrono::nanoseconds>(next_send_time.time_since_epoch());
        //string next_send_time_str = to_string(next_send_time_ns.count());

        NodeAddr dst_addr = other_addrs[i % other_addrs.size()];

        msg_info info;
        info.length = msg_len;
        info.src_ip = my_addr.ip;
        info.src_mac = my_addr.mac;
        info.portid = 0;
        info.dst_ip = dst_addr.ip;
        info.dst_mac = dst_addr.mac;

        buffer[0] = REQUEST;
        sprintf(&buffer[1], "%llu", (unsigned long long) next_send_time_ns.count());
        //strcpy(&buffer[1], next_send_time_str.c_str());
        // for (int b = 2 + next_send_time_str.size(); b < msg_len; b++)
        //     buffer[b] = (i + b) % 256;

        while(chrono::high_resolution_clock::now() < next_send_time)
            continue;

        while (send_dpdk(buffer, &info) < 0)
            continue;
    }
}

unsigned long long worker_loop(const NodeAddr &my_addr, const vector<NodeAddr> &other_addrs, int num_msgs, int msg_len, double mean)
{
    unsigned char send_buffer[MAX_MSG_SIZE];
    int64_t total_latency = 0;
    default_random_engine generator(0);
    exponential_distribution<float>distribution(mean);
    auto next_send_time = chrono::high_resolution_clock::now();
    int received = 0;

    for (int i = 0; i < num_msgs; i++)
    {
        chrono::nanoseconds delay((long long int) round(distribution(generator)));
        next_send_time += delay;
        auto next_send_time_ns = chrono::duration_cast<chrono::nanoseconds>(next_send_time.time_since_epoch());

        NodeAddr dst_addr = other_addrs[i % other_addrs.size()];

        msg_info send_info;
        send_info.length = msg_len;
        send_info.src_ip = my_addr.ip;
        send_info.src_mac = my_addr.mac;
        send_info.portid = 0;
        send_info.dst_ip = dst_addr.ip;
        send_info.dst_mac = dst_addr.mac;

        send_buffer[0] = REQUEST;
        sprintf((char*) &send_buffer[1], "%llu", (unsigned long long) next_send_time_ns.count());

        while(chrono::high_resolution_clock::now() < next_send_time){
            unsigned char recv_buffer[MAX_MSG_SIZE];
            msg_info recv_info;
            if (recv_dpdk(recv_buffer, &recv_info, NULL) != 0){
                if (recv_buffer[0] == RESPONSE)
                {
                    // receive echo response
                    auto recv_time = chrono::high_resolution_clock::now();
                    auto recv_time_ns = chrono::duration_cast<chrono::nanoseconds>(recv_time.time_since_epoch());
                    total_latency += (recv_time_ns.count()) - stoull((char*) &recv_buffer[1]);
                    received++;
                }
                else if (recv_buffer[0] == REQUEST)
                {
                    // send a echo response to a request
                    swap(recv_info.dst_ip, recv_info.src_ip);
                    swap(recv_info.dst_mac, recv_info.src_mac);
                    recv_buffer[0] = RESPONSE;

                    while (send_dpdk(recv_buffer, &recv_info) < 0)
                        continue;
                }
            }
        }

        while (send_dpdk(send_buffer, &send_info) < 0)
            continue;
    }

    while (received < num_msgs){
            unsigned char recv_buffer[MAX_MSG_SIZE];
            msg_info recv_info;
            while (recv_dpdk(recv_buffer, &recv_info, NULL) == 0)
                continue;

            if (recv_buffer[0] == RESPONSE)
            {
                // receive echo response
                auto recv_time = chrono::high_resolution_clock::now();
                auto recv_time_ns = chrono::duration_cast<chrono::nanoseconds>(recv_time.time_since_epoch());
                total_latency += (recv_time_ns.count()) - stoull((char*) &recv_buffer[1]);
                received++;
            }
            else if (recv_buffer[0] == REQUEST)
            {
                // send a echo response to a request
                swap(recv_info.dst_ip, recv_info.src_ip);
                swap(recv_info.dst_mac, recv_info.src_mac);
                recv_buffer[0] = RESPONSE;

                while (send_dpdk(recv_buffer, &recv_info) < 0)
                    continue;
            }
    }

    return total_latency / num_msgs;
}

void done_loop()
{
    unsigned char buffer[MAX_MSG_SIZE];
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

void controller_loop(const NodeAddr &my_addr, const vector<NodeAddr> &other_addrs, double mean_start, double mean_end, double mean_increment){
    for(double mean = mean_start; mean <= mean_end; mean += mean_increment) {
        cout << "sending start msgs for mean: " << mean << endl;
        for (auto addr : other_addrs)
        {
            send_msg(my_addr, addr, START, to_string(mean));
        }

        cout << "waiting for done messages" << endl;
        wait_for_done_msgs(other_addrs);

        cout << "sending done messages" << endl;
        for (auto addr : other_addrs)
        {
            send_msg(my_addr, addr, DONE, "");
        }
    }

    cout << "sending terminate messages" << endl;
    for (auto addr : other_addrs)
    {
        send_msg(my_addr, addr, TERMINATE, "");
    }
}

int main(int argc, char *argv[])
{
    int ret = init_dpdk(argc, argv, F_SINGLE_RECV);
    argc -= ret;
    argv += ret;

    bool f_flag = false, s_flag = false, e_flag = false;
    string filename;
    bool is_controller = false;
    int num_msgs = 1000;
    int msg_len = 1000;
    double mean_start = 0;
    double mean_end = 0;
    double mean_increment = 1;

    char c;
    while ((c = getopt(argc, argv, "f:cn:l:s:e:i:")) != -1)
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
        case 's':
            s_flag = true;
            mean_start = atof(optarg);
            break;
        case 'e':
            e_flag = true;
            mean_end = atof(optarg);
            break;
        case 'i':
            mean_increment = atof(optarg);
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

        if (!s_flag)
        {
            cerr << "missing -s for poisson mean start" << endl;
            exit(1);
        }
        else if(mean_start <= 0){
            cerr << "poisson mean start must be greater than 0: " << mean_start << endl;
            exit(1);
        }

        if (!e_flag)
        {
            cerr << "missing -e for poisson mean end" << endl;
            exit(1);
        }
        else if(mean_end < mean_start){
            cerr << "poisson mean end must be greater than or equal to mean start, start: " << mean_start << " end: " << mean_end << endl;
            exit(1);
        }

        if(mean_increment <= 0){
            cerr << "poisson mean increment must be greater than 0: " << mean_increment << endl;
            exit(1);
        }

        cout << "starting controller" << endl;
        controller_loop(my_addr, other_addrs, mean_start, mean_end, mean_increment);

        cout << "controller exiting" << endl;
        return 0;
    }

    cout << "starting worker nodes" << endl;

    while (true) {
        cout << "waiting for start or terminate msg" << endl;
        NodeAddr controller_addr;
        MsgType type;
        string content;
        wait_for_msg(controller_addr, type, content);

        if (type == TERMINATE)
            break;
        else if (type != START) {
            cerr << "expecting start or terminate msg, received " << type << endl;
            exit(1);
        }

        sleep(2);

        double mean = stod(content);
        cout << "starting main loop with mean: " << mean << endl;
        unsigned long long avg_latency = worker_loop(my_addr, other_addrs, num_msgs, msg_len, mean);

        cout << "sending done msg" << endl;
        send_msg(my_addr, controller_addr, DONE, to_string(avg_latency));

        cout << "starting done loop" << endl;
        done_loop();
    }

    cout << "worker terminated" << endl;
    terminate_dpdk();

    return 0;
}