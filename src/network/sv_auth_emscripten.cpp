#include "network.h"
#include "sv_auth.h"

void NETWORK_AUTH_Construct( void )
{
	// Browser build: auth server is not used.
}

void NETWORK_AUTH_Destruct( void )
{
	// No-op.
}

NETADDRESS_s NETWORK_AUTH_GetServerAddress( void )
{
	NETADDRESS_s addr;
	addr.Clear();
	return addr;
}

NETADDRESS_s NETWORK_AUTH_GetCachedServerAddress( void )
{
	NETADDRESS_s addr;
	addr.Clear();
	return addr;
}

void SERVER_AUTH_ParsePacket( BYTESTREAM_s * )
{
	// No-op.
}

void SERVER_InitClientSRPData ( const ULONG )
{
	// No-op.
}

bool SERVER_ProcessSRPClientCommand( LONG, BYTESTREAM_s * )
{
	return false;
}
