#include "Transport/McpListenSocket.h"

#if PLATFORM_WINDOWS || PLATFORM_MAC || PLATFORM_LINUX
#include "BSDSockets/SocketsBSD.h"
#endif

bool EngineWire::ConfigureListenSocket(FSocket& Socket)
{
#if PLATFORM_WINDOWS || PLATFORM_MAC || PLATFORM_LINUX
	// These platform socket subsystems create FSocketBSD-derived sockets.
	const auto Handle = static_cast<FSocketBSD&>(Socket).GetNativeSocket();
	const int Enabled = 1;
#if PLATFORM_WINDOWS
	// Winsock reuse can allow two live TCP listeners on the same endpoint.
	return setsockopt(Handle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
		reinterpret_cast<const char*>(&Enabled), sizeof(Enabled)) == 0;
#else
	// UE's BSD SetReuseAddr can also enable SO_REUSEPORT. Only enable address
	// reuse here so a closed connection can leave TIME_WAIT without blocking a restart.
	return setsockopt(Handle, SOL_SOCKET, SO_REUSEADDR, &Enabled, sizeof(Enabled)) == 0;
#endif
#else
	// Use the platform's default bind policy without enabling port reuse.
	return true;
#endif
}
