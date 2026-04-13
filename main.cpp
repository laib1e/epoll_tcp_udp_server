#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <cstring>
#include <unistd.h>
#include <iostream>

constexpr int MAX_EVENTS = 10;

int main() 
{
    int server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_socket == -1) return -1;
    {
        int opt = 1;
        setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    }

    sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(60000);
    if (bind(server_socket, (struct sockaddr*)&address, sizeof(address)) < 0) 
    {
        perror("Bind failed");
        close(server_socket);
        return -1;
    }
    listen(server_socket, SOMAXCONN);

    int udp_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp_socket == -1) return -1;

    sockaddr_in udp_address;
    udp_address.sin_family = AF_INET;
    udp_address.sin_addr.s_addr = INADDR_ANY;
    udp_address.sin_port = htons(60001);
    if (bind(udp_socket, (struct sockaddr*)&udp_address, sizeof(udp_address)) < 0) 
    {
        perror("Bind failed");
        close(udp_socket);
        return -1;
    }

    int epoll_fd = epoll_create1(0);
    struct epoll_event ev{}, events[MAX_EVENTS];

    ev.events = EPOLLIN;
    ev.data.fd = server_socket;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_socket, &ev);

    ev.events = EPOLLIN;
    ev.data.fd = udp_socket;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, udp_socket, &ev);

    while (true) 
    {
        int wait = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

        for (int i = 0; i < wait; i++) 
        {
            if (events[i].data.fd == server_socket) 
            {
                int conn_sock = accept(server_socket, nullptr, nullptr);
                if (conn_sock == -1) continue;

                ev.events = EPOLLIN;
                ev.data.fd = conn_sock;
                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conn_sock, &ev);
            } else if (events[i].data.fd == udp_socket) {
                char buf[1024];
                sockaddr_in udp_client_address;
                socklen_t len = sizeof(udp_client_address);
                int n = recvfrom(events[i].data.fd, buf, sizeof(buf), 0, (struct sockaddr*)&udp_client_address, &len);
                if (n > 0) 
                {
                    std::cout << "UDP received: " << n << " bytes" << std::endl;
                    sendto(events[i].data.fd, buf, n, 0, (const struct sockaddr*)&udp_client_address, len);
                }
            } else {
                char buf[1024];
                int n = read(events[i].data.fd, buf, sizeof(buf));
                if (n <= 0) 
                {
                    close(events[i].data.fd);
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, events[i].data.fd, nullptr);
                } else {
                    write(events[i].data.fd, buf, n);
                }
            }


        }
    }
}