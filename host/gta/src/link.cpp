#include "link.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "gtr_frame.h"

namespace
{
	constexpr DWORD kRetryMilliseconds = 1000;
	// A guest that stops reading is dropped instead of filling memory
	constexpr size_t kMaxOutgoingBytes = 8 << 20;
	constexpr size_t kMaxLineBytes = 1 << 20;
}

GuestLink::GuestLink() :
	mSocket(INVALID_SOCKET)
{
	WSADATA data;
	WSAStartup(MAKEWORD(2, 2), &data);
}

GuestLink::~GuestLink()
{
	drop();
	WSACleanup();
}

void GuestLink::drop()
{
	if (mSocket != INVALID_SOCKET)
		closesocket(mSocket);
	mSocket = INVALID_SOCKET;
	mState = State::Idle;
	mNextAttempt = GetTickCount() + kRetryMilliseconds;
	mIncoming.clear();
	mOutgoing.clear();
}

std::vector<std::string> GuestLink::poll()
{
	std::vector<std::string> lines;

	if (mState == State::Idle)
	{
		if (static_cast<int32_t>(GetTickCount() - mNextAttempt) < 0)
			return lines;
		mSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (mSocket == INVALID_SOCKET)
		{
			mNextAttempt = GetTickCount() + kRetryMilliseconds;
			return lines;
		}
		// Non-blocking before connecting: Windows takes a second or two to refuse a connection to a port nobody listens on
		u_long enabled = 1;
		ioctlsocket(mSocket, FIONBIO, &enabled);
		sockaddr_in address = {};
		address.sin_family = AF_INET;
		address.sin_port = htons(GTR_LINK_PORT);
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (connect(mSocket, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0)
			mState = State::Connected;
		else if (WSAGetLastError() == WSAEWOULDBLOCK)
			mState = State::Connecting;
		else
			drop();
		if (mState == State::Connected)
			++mGeneration;
		return lines;
	}

	if (mState == State::Connecting)
	{
		fd_set writable, failed;
		FD_ZERO(&writable);
		FD_ZERO(&failed);
		FD_SET(mSocket, &writable);
		FD_SET(mSocket, &failed);
		const timeval now = {0, 0};
		if (select(0, nullptr, &writable, &failed, &now) <= 0)
			return lines;
		if (FD_ISSET(mSocket, &failed))
		{
			drop();
			return lines;
		}
		const BOOL noDelay = TRUE;
		setsockopt(mSocket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&noDelay), sizeof(noDelay));
		mState = State::Connected;
		++mGeneration;
	}

	bool closed = false;
	char buffer[16384];
	for (;;)
	{
		const int received = recv(mSocket, buffer, sizeof(buffer), 0);
		if (received > 0)
		{
			mIncoming.append(buffer, static_cast<size_t>(received));
			continue;
		}
		closed = received == 0 || WSAGetLastError() != WSAEWOULDBLOCK;
		break;
	}
	size_t start = 0;
	for (size_t end = mIncoming.find('\n'); end != std::string::npos; end = mIncoming.find('\n', start))
	{
		if (end > start)
			lines.emplace_back(mIncoming, start, end - start);
		start = end + 1;
	}
	mIncoming.erase(0, start);
	if (closed || mIncoming.size() > kMaxLineBytes)
		drop();
	else
		send_line({});
	return lines;
}

void GuestLink::send_line(const std::string &line)
{
	if (mState != State::Connected)
		return;
	if (!line.empty())
	{
		mOutgoing += line;
		mOutgoing += '\n';
	}
	while (!mOutgoing.empty())
	{
		const int sent = send(mSocket, mOutgoing.data(), static_cast<int>(mOutgoing.size()), 0);
		if (sent > 0)
			mOutgoing.erase(0, static_cast<size_t>(sent));
		else if (WSAGetLastError() == WSAEWOULDBLOCK)
			break;
		else
		{
			drop();
			return;
		}
	}
	if (mOutgoing.size() > kMaxOutgoingBytes)
		drop();
}
