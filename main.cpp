#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <algorithm>
#include <vector>
#include <ctime>
#include <concepts>
#include <format>

struct Stats 
{
    bool running = true;
    int total_connections = 0;
    int active_connections = 0;

    std::string process(std::string_view cmd) 
    {
        if (cmd == "shutdown") 
        {
            running = false;
            return "server shutting down";
        }
        if (cmd == "stats") 
        {
            return std::format("total={}, active={}", total_connections, active_connections);
        }
        if (cmd == "time") 
        {
            char buf[80];
            std::time_t now = std::time(nullptr);
            std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
            return buf;
        }
        return "unknow command";
    }
};

template<typename T>
concept TransportPolicy = requires(T t) 
{
    { t.fd() } -> std::convertible_to<int>;
    { t.owns(int{}) } -> std::convertible_to<bool>;
    { t.handle_event(int{}, std::declval<Stats&>(), int{}) };
};

class TCP 
{
private:
    int tcp_fd_;
    std::vector<int> clients_;
public:
    explicit TCP(int port) 
    {
        tcp_fd_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (tcp_fd_ == -1) throw std::runtime_error("OPEN TCP SOCKET ERROR");
        {
            int opt = 1;
            setsockopt(tcp_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        }
        sockaddr_in address;
        {
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = INADDR_ANY;
            address.sin_port = htons(port);
        }
        if (bind(tcp_fd_, (struct sockaddr*)&address, sizeof(address)) < 0)
        {
            close(tcp_fd_);
            throw std::runtime_error("BIND TCP FAILED");
        }
        listen(tcp_fd_, SOMAXCONN);
    }

    ~TCP() 
    {
        for (const auto& client : clients_)
        {
            close(client);
        } 
        close(tcp_fd_);
    }

    int fd() 
    {
        return tcp_fd_;
    }

    bool owns(int fd) 
    {
        return fd == tcp_fd_ || std::find(clients_.begin(), clients_.end(), fd) != clients_.end();
    }

    void handle_event(int fd, Stats& stats, int epoll_fd) 
    {
        if (fd == tcp_fd_) 
        {
            int conn_sock = accept(tcp_fd_, nullptr, nullptr);
            if (conn_sock == -1) return;

            struct epoll_event ev{};
            ev.events = EPOLLIN;
            ev.data.fd = conn_sock;
            epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conn_sock, &ev);
            clients_.push_back(conn_sock);
            stats.total_connections++;
            stats.active_connections++;
        } else {
            char buf[1024];
            int n = read(fd, buf, sizeof(buf));
            if (n <= 0) 
            {
                close(fd);
                clients_.erase(std::remove(clients_.begin(), clients_.end(), fd), clients_.end());
                stats.active_connections--;
            } else {
                if (buf[0] == '/') 
                {
                    if (n > 0 and buf[n - 1] == '\n') buf[n - 1] = '\0';
                    else buf[n] = '\0';
                    std::string response = stats.process(buf + 1);
                    write(fd, response.data(), response.length());
                } else {
                    write(fd, buf, n);
                }
            }
        }
    }
};

class UDP
{
private:
    int udp_fd_;
public:
    explicit UDP(int port) 
    {
        udp_fd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (udp_fd_ == -1) throw std::runtime_error("OPEN UDP SOCKET ERROR");
        sockaddr_in address;
        {
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = INADDR_ANY;
            address.sin_port = htons(port);
        }
        if (bind(udp_fd_, (struct sockaddr*)&address, sizeof(address)) < 0) 
        {
            close(udp_fd_);
            throw std::runtime_error("BIND UDP FAILED");
        }
    }

    ~UDP() 
    {
        close(udp_fd_);
    }

    int fd() 
    {
        return udp_fd_;
    }

    bool owns(int fd) 
    {
        return fd == udp_fd_;
    }

    void handle_event(int fd, Stats& stats, int epoll_fd) 
    {
        if (fd == udp_fd_) 
        {
            char buf[1024];
            sockaddr_in address;
            socklen_t len = sizeof(address);
            int n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr*)&address, &len);
            if (n > 0) 
            {
                if (buf[0] == '/') 
                {
                    if (n > 0 and buf[n - 1] == '\n') buf[n - 1] = '\0';
                    else buf[n] = '\0';
                    std::string response = stats.process(buf + 1);
                    sendto(fd, response.data(), response.length(), 0, (const sockaddr*)&address, len);
                } else {
                    sendto(fd, buf, n, 0, (const sockaddr*)&address, len);
                }
            }
        }
    }
};

template<TransportPolicy T, TransportPolicy U, int MaxEvents = 10>
class EventLoop 
{
private:
    int epoll_fd_;
    T& tcp_;
    U& udp_;
public:
    explicit EventLoop(T& tcp, U& udp) : tcp_(tcp), udp_(udp)
    {
        epoll_fd_ = epoll_create1(0);

        struct epoll_event ev{};

        ev.events = EPOLLIN;
        ev.data.fd = tcp_.fd();
        epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, tcp_.fd(), &ev);

        ev.events = EPOLLIN;
        ev.data.fd = udp_.fd();
        epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, udp_.fd(), &ev);
    }

    ~EventLoop() 
    {
        close(epoll_fd_);
    }

    void run(Stats &stats) 
    {
        epoll_event events[MaxEvents];
        while(stats.running) 
        {
            int wait = epoll_wait(epoll_fd_, events, MaxEvents, -1);
            for (int i = 0; i < wait; i++) 
            {
                int fd = events[i].data.fd;
                if (tcp_.owns(fd)) tcp_.handle_event(fd, stats, epoll_fd_);
                else if (udp_.owns(fd)) udp_.handle_event(fd, stats, epoll_fd_);
            }
        }
    }
};

int main() 
{
    Stats stats;
    TCP tcp_server(60000);
    UDP udp_server(60001);
    EventLoop<TCP, UDP> eventLoop(tcp_server, udp_server);
    eventLoop.run(stats);
}