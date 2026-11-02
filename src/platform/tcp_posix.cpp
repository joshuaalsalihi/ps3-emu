#include "platform/tcp.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

std::unique_ptr<TcpConnection> TcpConnection::accept_one(u16 port, std::string& error) {
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        error = std::strerror(errno);
        return nullptr;
    }
    const int yes = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(listener, 1) != 0) {
        error = std::strerror(errno);
        close(listener);
        return nullptr;
    }

    const int client = accept(listener, nullptr, nullptr);
    close(listener);
    if (client < 0) {
        error = std::strerror(errno);
        return nullptr;
    }
    // Packets are small and each waits for an answer, so send them at once.
    setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    return std::unique_ptr<TcpConnection>(new TcpConnection(client));
}

TcpConnection::~TcpConnection() {
    close(socket_);
}

std::optional<char> TcpConnection::read(bool wait) {
    pollfd request{socket_, POLLIN, 0};
    if (poll(&request, 1, wait ? -1 : 0) <= 0) return std::nullopt;
    char byte = 0;
    if (recv(socket_, &byte, 1, 0) != 1) return std::nullopt;
    return byte;
}

void TcpConnection::write(std::string_view bytes) {
    while (!bytes.empty()) {
        const ssize_t sent = send(socket_, bytes.data(), bytes.size(), 0);
        if (sent <= 0) return; // the client is gone; the next read says so
        bytes.remove_prefix(static_cast<std::size_t>(sent));
    }
}
