#pragma once

// A TCP connection to one client, for the GDB server. It listens on the loopback address only,
// so nothing outside this machine can connect.

#include "common/types.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>

class TcpConnection {
public:
    // Listens on 127.0.0.1:`port` and waits for one client. Returns null with `error` set if
    // the port can't be used.
    static std::unique_ptr<TcpConnection> accept_one(u16 port, std::string& error);
    ~TcpConnection();

    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    // The next byte. With `wait`, waits for one and returns nothing once the client has closed
    // the connection; without, returns nothing if no byte is ready.
    std::optional<char> read(bool wait);
    void write(std::string_view bytes);

private:
    explicit TcpConnection(int socket) : socket_(socket) {}

    int socket_;
};
