#include "network.h"
#include "cl_auth.h"
#include "c_cvars.h"

CVAR( Bool, cl_hideaccount, false, CVAR_ARCHIVE )

void CLIENT_ProcessSRPServerCommand( LONG, BYTESTREAM_s * )
{
	// SRP-based account auth is not available in the browser build.
}

void CLIENT_LogOut( void )
{
	// No-op.
}

bool CLIENT_IsLoggedIn( void )
{
	return false;
}

#ifdef ENABLE_AUTH_STORAGE
void CLIENT_RetrieveUserAndLogIn( const FString )
{
	// No-op.
}
#endif
