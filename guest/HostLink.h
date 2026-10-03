// The link the guest is driven over: one JSON object per line, TCP on 127.0.0.1. The guest listens, so clients can come and go:
// the host game's script, and beside it the tools that inspect or direct a session while it runs.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Gtr {
class HostLink {
public:
    struct Received {
        int Client;
        std::string Line;
    };

    HostLink();
    ~HostLink();

    HostLink(const HostLink &) = delete;
    HostLink &operator=(const HostLink &) = delete;

    bool Listen(uint16_t port);
    // The complete lines that arrived since the last call, with who sent each; never blocks
    std::vector<Received> Poll();
    // Nothing happens when the client has gone. A skippable line is one a later line of its kind makes stale, like where
    // something is now: it is left out while the client has unread lines waiting.
    void Send(int client, const std::string &line, bool skippable = false);
    void Broadcast(const std::string &line, int exceptClient = 0);
    bool IsConnected() const;
    // The clients that connected since the last call
    std::vector<int> TakeConnected();
private:
    struct Client {
        int Id;
        uintptr_t Socket;
        std::string Incoming;
        std::string Outgoing;
        bool Closed { false };
    };

    void Flush(Client &client);

    uintptr_t mListener;
    std::vector<Client> mClients;
    std::vector<int> mJustConnected;
    int mNextId { 1 };
};
}
