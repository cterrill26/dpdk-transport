#include <string>
#include <cstring>
#include <iostream>
#include <fstream>
#include <unistd.h>
#include <vector>
#include <unordered_set>
#include <chrono>
#include <random>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <thread>

using namespace std;

#define DISTR_SAMPLE_SIZE 1000
#define MAX_MSG_SIZE 100000
#define UDP_PORT_DEFAULT 8000
#define UDP_PORT_SENDER 8001
#define MAX_RUNTIME_S 20


enum MsgType
{
    START,
    DONE,
    REQUEST,
    RESPONSE,
    TERMINATE,
    STOP
};

struct SendThreadParams
{
    const in_addr_t &my_addr;
    const vector<in_addr_t> other_addrs;
    const int num_msgs;
    const int msg_len;
    const double mean;
    volatile unsigned long long duration;
};

struct WorkerResult{
    unsigned long long latency;
    unsigned long long duration;
    int received;
};

vector<in_addr_t> get_addrs_from_file(const string &filename, in_addr_t &my_addr);
void wait_for_msg(const int &sockfd, in_addr_t &addr, MsgType &type, string &content);
void send_msg(const int &sockfd, const in_addr_t &dst_addr, const MsgType &type, const string &body, const double &mean);
int send_thread(SendThreadParams *params);
void wait_for_done_msgs(const int &sockfd, const vector<in_addr_t> &addrs);
WorkerResult worker_loop(const int &sockfd, const in_addr_t &my_addr, const vector<in_addr_t> &other_addrs, const int &num_msgs, const int &msg_len);
void done_loop(const int &sockfd);
void controller_loop(const int &sockfd, const vector<in_addr_t> &other_addrs, const double &mean_start, const double &mean_end, const double &mean_increment);

// lines in address file are assumed to be formated
// as "192.168.123.132,00:1b:63:84:45:e6" for ip and mac address,
// the first address will be the node's own addresses
vector<in_addr_t> get_addrs_from_file(const string &filename, in_addr_t &my_addr)
{
    ifstream f;
    vector<in_addr_t> addrs;
    bool first = true;

    f.open(filename, ios::in);
    if (f.is_open())
    {
        string ip, mac;
        while (getline(f, ip, ',') && getline(f, mac))
        {
            in_addr_t addr = inet_addr(ip.c_str());

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

void wait_for_msg(const int &sockfd, in_addr_t &addr, MsgType &type, string &content)
{
    char buffer[MAX_MSG_SIZE];
    struct sockaddr_in src_addr;
    memset(&src_addr, 0, sizeof(src_addr));
    socklen_t addrlen = sizeof(src_addr);
    recvfrom(sockfd, buffer, MAX_MSG_SIZE, 0, (struct sockaddr *) &src_addr, &addrlen);

    addr = src_addr.sin_addr.s_addr;
    type = ((MsgType)buffer[0]);
    content = &buffer[1];
}

void send_msg(const int &sockfd, const in_addr_t &dst_addr, MsgType type, const string &content)
{
    char buffer[content.length() + 2];
    buffer[0] = type;
    strcpy(&buffer[1], content.c_str());

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_addr.s_addr = dst_addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(UDP_PORT_DEFAULT);
    socklen_t len = sizeof(addr);

    sendto(sockfd, (const char *)buffer, sizeof(buffer),
               0, (const struct sockaddr *)&addr,
               len);
}

void wait_for_done_msgs(const int &sockfd, const vector<in_addr_t> &addrs)
{
    unordered_set<in_addr_t> ips;
    for (auto addr : addrs)
    {
        ips.insert(addr);
    }

    unsigned char buffer[MAX_MSG_SIZE];
    unsigned long long total_latency = 0;
    unsigned long long total_duration = 0;
    int total_received = 0;

    fd_set readfds;
    struct timeval timeout;
    int result;

    FD_ZERO(&readfds);
    FD_SET(sockfd, &readfds);


    while (!ips.empty())
    {
        timeout.tv_sec = MAX_RUNTIME_S;
        timeout.tv_usec = 0;
        result = select(sockfd + 1, &readfds, NULL, NULL, &timeout);

        if (result == 0) {
            // timeout occurred
            cout << "timeout occured, sending stop messages" << endl;
            for (auto addr : ips) {
                send_msg(sockfd, addr, STOP, "");
            }

            continue;
        }
        else if (result < 0){
            cerr << "socket read error" << endl;
            exit(1);
        }
        else if (!FD_ISSET(sockfd, &readfds)){
            continue;
        }

        struct sockaddr_in src_addr;
        memset(&src_addr, 0, sizeof(src_addr));
        socklen_t addrlen = sizeof(src_addr);
        recvfrom(sockfd, buffer, MAX_MSG_SIZE, 0, (struct sockaddr *) &src_addr, &addrlen);

        if (buffer[0] != DONE)
        {
            cerr << "Controller node received incorrect done message: " << buffer[0] << endl;
            exit(1);
        }

        unsigned long long latency;
        unsigned long long duration;
        int received;
        sscanf((char *) &buffer[1], "%llu,%llu,%d", &duration, &latency, &received);
        cout << "Single node's latency: " << latency << endl;
        cout << "Single node's duration: " << duration << endl;
        cout << "Single node's received: " << received << endl;
        total_latency += latency;
        total_duration += duration;
        total_received += received;
        ips.erase(src_addr.sin_addr.s_addr);
    }

    cout << "Total latency: " << total_latency << endl;
    cout << "Total duration: " << total_duration << endl;
    cout << "Total received: " << total_received << endl;
}

int send_thread(SendThreadParams *params)
{
    long long int distr_samples[DISTR_SAMPLE_SIZE];
    default_random_engine generator(0);
    exponential_distribution<float> distribution(1 / params->mean);
    for (int i = 0; i < DISTR_SAMPLE_SIZE; i++)
    {
        distr_samples[i] = (long long int)round(distribution(generator));
    }

    int send_sockfd;
    struct sockaddr_in servaddr;
    if ((send_sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        cerr << "send socket creation failed" << endl;
        exit(1);
    }

    // Filling server information
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = params->my_addr;
    servaddr.sin_port = htons(UDP_PORT_SENDER);

    // Bind the socket with the server address
    if (bind(send_sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        cerr << "bind failed" << endl;
        exit(EXIT_FAILURE);
    }

    unsigned char send_buffer[MAX_MSG_SIZE];
    auto send_start = chrono::high_resolution_clock::now();

    for (int i = 0; i < params->num_msgs; i++)
    {
        chrono::nanoseconds delay(distr_samples[i % DISTR_SAMPLE_SIZE]);
        auto next_send_time = chrono::high_resolution_clock::now() + delay;
        long long unsigned next_send_time_ns = chrono::duration_cast<chrono::nanoseconds>(next_send_time.time_since_epoch()).count();

        in_addr_t dst_addr = params->other_addrs[i % params->other_addrs.size()];

        send_buffer[0] = REQUEST;
        *((long long unsigned *)&send_buffer[1]) = next_send_time_ns;

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_addr.s_addr = dst_addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(UDP_PORT_DEFAULT);
        socklen_t len = sizeof(addr);

        while (chrono::high_resolution_clock::now() < next_send_time)
        {
            continue;
        }

        sendto(send_sockfd, (const char *)send_buffer, params->msg_len,
                0, (const struct sockaddr *)&addr,
                len);
    }

    auto send_finish = chrono::high_resolution_clock::now();
    params->duration = chrono::duration_cast<chrono::nanoseconds>(send_finish - send_start).count();
    close(send_sockfd);

    return 0;
}

WorkerResult worker_loop(const int &sockfd, const in_addr_t &my_addr, const vector<in_addr_t> &other_addrs, const int &num_msgs, const int &msg_len, const double &mean)
{
    int64_t total_latency = 0;
    SendThreadParams params = {
        .my_addr = my_addr,
        .other_addrs = other_addrs,
        .num_msgs = num_msgs,
        .msg_len = msg_len,
        .mean = mean,
        .duration = 0};

    thread sender(send_thread, &params);

    for (int i = 0; i < num_msgs; i++)
    {
        while (true)
        {
            unsigned char recv_buffer[MAX_MSG_SIZE];

            struct sockaddr_in src_addr;
            memset(&src_addr, 0, sizeof(src_addr));
            socklen_t addrlen = sizeof(src_addr);
            ssize_t n = recvfrom(sockfd, recv_buffer, MAX_MSG_SIZE, 0, (struct sockaddr *) &src_addr, &addrlen);

            if (recv_buffer[0] == RESPONSE)
            {
                // receive echo response
                auto recv_time = chrono::high_resolution_clock::now();
                long long unsigned recv_time_ns = chrono::duration_cast<chrono::nanoseconds>(recv_time.time_since_epoch()).count();
                total_latency += recv_time_ns - *((long long unsigned *)&recv_buffer[1]);
                break;
            }
            else if (recv_buffer[0] == REQUEST)
            {
                // send a echo response to a request
                recv_buffer[0] = RESPONSE;
                src_addr.sin_port = htons(UDP_PORT_DEFAULT);
                sendto(sockfd, (const char *)recv_buffer, n,
                        0, (const struct sockaddr *)&src_addr,
                        addrlen);
            }
            else if (recv_buffer[0] == STOP)
            {
                sender.join();

                WorkerResult result;
                result.duration = params.duration;
                result.latency = total_latency;
                result.received = i;

                return result;
            }
        }
    }

    sender.join();

    WorkerResult result;
    result.duration = params.duration;
    result.latency = total_latency;
    result.received = num_msgs;

    return result;
}

void done_loop(const int &sockfd)
{
    unsigned char buffer[MAX_MSG_SIZE];

    while (true)
    {
        struct sockaddr_in src_addr;
        memset(&src_addr, 0, sizeof(src_addr));
        socklen_t addrlen = sizeof(src_addr);
        ssize_t n = recvfrom(sockfd, buffer, MAX_MSG_SIZE, 0, (struct sockaddr *) &src_addr, &addrlen);

        if (buffer[0] == REQUEST)
        {
            // send a echo response to a request
            buffer[0] = RESPONSE;
            src_addr.sin_port = htons(UDP_PORT_DEFAULT);
            sendto(sockfd, (const char *)buffer, n,
                    0, (const struct sockaddr *)&src_addr,
                    addrlen);
        }
        else if (buffer[0] == DONE)
        {
            break;
        }
    }
}

void controller_loop(const int &sockfd, const vector<in_addr_t> &other_addrs, const double &mean_start, const double &mean_end, const double &mean_increment)
{
    for (double mean = mean_start; mean <= mean_end; mean += mean_increment)
    {
        cout << "sending start msgs for mean: " << mean << endl;
        for (auto addr : other_addrs)
        {
            send_msg(sockfd, addr, START, to_string(mean));
        }

        cout << "waiting for done messages" << endl;
        wait_for_done_msgs(sockfd, other_addrs);

        cout << "sending done messages" << endl;
        for (auto addr : other_addrs)
        {
            send_msg(sockfd, addr, DONE, "");
        }

        sleep(2);
    }

    cout << "sending terminate messages" << endl;
    for (auto addr : other_addrs)
    {
        send_msg(sockfd, addr, TERMINATE, "");
    }
}

int main(int argc, char *argv[])
{
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

    in_addr_t my_addr;
    auto other_addrs = get_addrs_from_file(filename, my_addr);

    int sockfd;
    struct sockaddr_in servaddr;
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        cerr << "socket creation failed" << endl;
        exit(1);
    }

    // Filling server information
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = my_addr;
    servaddr.sin_port = htons(UDP_PORT_DEFAULT);

    // Bind the socket with the server address
    if (bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        cerr << "bind failed" << endl;
        exit(EXIT_FAILURE);
    }

    if (is_controller)
    {
        // this nodes simply starts the other nodes and collects
        // performance measurements at the end

        if (!s_flag)
        {
            cerr << "missing -s for poisson mean start" << endl;
            exit(1);
        }
        else if (mean_start <= 0)
        {
            cerr << "poisson mean start must be greater than 0: " << mean_start << endl;
            exit(1);
        }

        if (!e_flag)
        {
            cerr << "missing -e for poisson mean end" << endl;
            exit(1);
        }
        else if (mean_end < mean_start)
        {
            cerr << "poisson mean end must be greater than or equal to mean start, start: " << mean_start << " end: " << mean_end << endl;
            exit(1);
        }

        if (mean_increment <= 0)
        {
            cerr << "poisson mean increment must be greater than 0: " << mean_increment << endl;
            exit(1);
        }

        cout << "starting controller" << endl;
        controller_loop(sockfd, other_addrs, mean_start, mean_end, mean_increment);

        cout << "controller exiting" << endl;
        return 0;
    }

    cout << "starting worker nodes" << endl;

    cout << "waiting for start or terminate msg" << endl;
    while (true)
    {
        in_addr_t controller_addr;
        MsgType type;
        string content;
        wait_for_msg(sockfd, controller_addr, type, content);

        if (type == TERMINATE)
            break;
        else if (type != START)
            continue;

        sleep(2);

        double mean = stod(content);
        cout << "starting main loop with mean: " << mean << endl;
        WorkerResult result = worker_loop(sockfd, my_addr, other_addrs, num_msgs, msg_len, mean);

        cout << "sending done msg" << endl;
        send_msg(sockfd, controller_addr, DONE, to_string(result.duration) + "," + to_string(result.latency) + "," + to_string(result.received));

        cout << "starting done loop" << endl;
        done_loop(sockfd);

        cout << "waiting for start or terminate msg" << endl;
    }

    cout << "worker terminated" << endl;
    close(sockfd);

    return 0;
}