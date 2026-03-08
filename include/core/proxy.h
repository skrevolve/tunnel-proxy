#pragma once

#include <string>

class Proxy {
public:
    Proxy(int local_port, const std::string& target_ip, int target_port);
    ~Proxy();
    
    void run();
    void stop();
    
private:
    int listen_fd_;
    std::string target_ip_;
    int target_port_;
    bool running_;
    
    // TODO: 구현 필요
    void accept_connection();
    int create_listening_socket(int port);
};
