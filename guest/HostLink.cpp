#include "HostLink.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>

using namespace Gtr;

namespace {
constexpr size_t sReadBytes = 65536;
// A client that stops reading is dropped instead of filling memory
constexpr size_t sMaxOutgoingBytes = 1 << 20;
constexpr size_t sMaxLineBytes = 1 << 20;
constexpr size_t sSkippableBacklogBytes = 16 << 10;

void SetNonBlocking(SOCKET socket) {
    u_long enabled = 1;
    ioctlsocket(socket, FIONBIO, &enabled);
}
}

HostLink::HostLink() :
    mListener(INVALID_SOCKET)
{
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
}

HostLink::~HostLink() {
    for (const Client &client : mClients)
        closesocket(client.Socket);
    if (mListener != INVALID_SOCKET)
        closesocket(mListener);
    WSACleanup();
}

bool HostLink::Listen(uint16_t port) {
    mListener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (mListener == INVALID_SOCKET)
        return false;

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(mListener, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0 || listen(mListener, SOMAXCONN) != 0) {
        closesocket(mListener);
        mListener = INVALID_SOCKET;
        return false;
    }
    SetNonBlocking(mListener);
    return true;
}

std::vector<HostLink::Received> HostLink::Poll() {
    std::vector<Received> lines;
    if (mListener == INVALID_SOCKET)
        return lines;

    for (SOCKET accepted = accept(mListener, nullptr, nullptr); accepted != INVALID_SOCKET; accepted = accept(mListener, nullptr, nullptr)) {
        SetNonBlocking(accepted);
        const BOOL noDelay = TRUE;
        setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&noDelay), sizeof(noDelay));
        mClients.push_back({ mNextId, accepted });
        mJustConnected.push_back(mNextId++);
    }

    char buffer[sReadBytes];
    for (Client &client : mClients) {
        // A client may send its last lines and close at once, so what was read is still handed over when the connection has ended
        for (;;) {
            const int received = recv(client.Socket, buffer, sizeof(buffer), 0);
            if (received > 0) {
                client.Incoming.append(buffer, (size_t)received);
                continue;
            }
            client.Closed = received == 0 || WSAGetLastError() != WSAEWOULDBLOCK;
            break;
        }

        size_t start = 0;
        for (size_t end = client.Incoming.find('\n'); end != std::string::npos; end = client.Incoming.find('\n', start)) {
            if (end > start)
                lines.push_back({ client.Id, client.Incoming.substr(start, end - start) });
            start = end + 1;
        }
        client.Incoming.erase(0, start);
        if (client.Incoming.size() > sMaxLineBytes)
            client.Closed = true;
        if (!client.Closed)
            Flush(client);
    }

    std::erase_if(mClients, [](const Client &client) {
        if (client.Closed)
            closesocket(client.Socket);
        return client.Closed;
    });
    return lines;
}

void HostLink::Flush(Client &client) {
    while (!client.Outgoing.empty()) {
        const int sent = send(client.Socket, client.Outgoing.data(), (int)client.Outgoing.size(), 0);
        if (sent > 0) {
            client.Outgoing.erase(0, (size_t)sent);
        } else {
            client.Closed = WSAGetLastError() != WSAEWOULDBLOCK;
            break;
        }
    }
    if (client.Outgoing.size() > sMaxOutgoingBytes)
        client.Closed = true;
}

void HostLink::Send(int client, const std::string &line, bool skippable) {
    const auto found = std::ranges::find(mClients, client, &Client::Id);
    if (found == mClients.end() || found->Closed)
        return;
    // A client that isn't reading (a host paused in its menu) is not sent what the next line of its kind will replace anyway,
    // or it would be dropped for a full buffer the moment it paused
    if (skippable && found->Outgoing.size() > sSkippableBacklogBytes)
        return;
    found->Outgoing += line;
    found->Outgoing += '\n';
    Flush(*found);
}

void HostLink::Broadcast(const std::string &line, int exceptClient) {
    for (Client &client : mClients) {
        if (client.Id == exceptClient || client.Closed)
            continue;
        client.Outgoing += line;
        client.Outgoing += '\n';
        Flush(client);
    }
}

bool HostLink::IsConnected() const {
    return !mClients.empty();
}

std::vector<int> HostLink::TakeConnected() {
    std::vector<int> connected;
    connected.swap(mJustConnected);
    return connected;
}
