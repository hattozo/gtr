// The host's end of the link to the guest: one JSON object per line over TCP on 127.0.0.1. Nothing here ever blocks, because it
// runs inside the game's script tick: connecting is spread over ticks, and what can't be sent at once waits in a buffer.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

class GuestLink
{
public:
	GuestLink();
	~GuestLink();

	GuestLink(const GuestLink &) = delete;
	GuestLink &operator=(const GuestLink &) = delete;

	/// Moves the connection along, sends what is waiting and returns the lines that arrived. Call once a tick.
	std::vector<std::string> poll();
	void send_line(const std::string &line);
	bool connected() const { return mState == State::Connected; }
	/// Counts connections, so a caller can tell a new one from the one it already introduced itself to.
	int generation() const { return mGeneration; }
private:
	enum class State
	{
		Idle,
		Connecting,
		Connected
	};

	void drop();

	uintptr_t mSocket;
	State mState = State::Idle;
	int mGeneration = 0;
	uint32_t mNextAttempt = 0;
	std::string mIncoming;
	std::string mOutgoing;
};
