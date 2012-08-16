/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include "q_stdinc.h"
#include "arch_def.h"
#include "net_sys.h"
#include "quakedef.h"
#include "net_defs.h"
#include "net_sdlnet.h"
#if defined(SDL_FRAMEWORK) || defined(NO_SDL_CONFIG)
#include <SDL/SDL.h>
#include <SDL_net/SDL_net.h>
#else
#include "SDL.h"
#include "SDL_net.h"
#endif

#define MAX_SOCKETS	32

static int		net_controlsocket;
static int		net_broadcastsocket = 0;
static int		net_acceptsocket = -1;
static struct qsockaddr	broadcastaddr;

SDLNet_SocketSet	acceptsocket_set;
IPaddress		myaddr;
// contains a map of socket numbers to SDL_net UDP sockets
UDPsocket		net_sockets[MAX_SOCKETS];

static int socket_id (UDPsocket socket_p)
{
	int		idx;

	for (idx = 0; idx < MAX_SOCKETS; idx++)
	{
		if (net_sockets[idx] == socket_p)
			return idx;
	}

	for (idx = 0; idx < MAX_SOCKETS; idx++)
	{
		if (net_sockets[idx] == NULL)
			break;
	}

	if (idx == MAX_SOCKETS)
		Sys_Error("net_sdlnet: No free sockets.");

	net_sockets[idx] = socket_p;

	return idx;
}

static const char *_AddrToString (int ip, int port)
{
	static char buffer[22];

	sprintf(buffer, "%d.%d.%d.%d:%d", (ip >> 24) & 0xff, (ip >> 16) & 0xff,
					  (ip >> 8) & 0xff, ip & 0xff, port);
	return buffer;
}

static const char *_IPAddrToString (IPaddress *address)
{
	int ip;
	int port;

	ip = SDLNet_Read32(&address->host);
	port = SDLNet_Read16(&address->port);

	return _AddrToString(ip, port);
}

int  SDLN_Init (void)
{
	int		i;
	IPaddress	*ipaddress;

	// init SDL
	if (SDLNet_Init() == -1)
	{
		Con_SafePrintf ("SDL_net initialization failed.\n");
		return -1;
	}

	// allocate a socket set for the accept socket
	acceptsocket_set = SDLNet_AllocSocketSet(1);
	if (acceptsocket_set == NULL)
	{
		Con_DPrintf ("SDL_net initialization failed: Could not create socket set.\n");
		return -1;
	}

	// set my IP address
	i = COM_CheckParm ("-ip");
	if (i)
	{
		if (i < com_argc-1)
		{
			SDLNet_ResolveHost(&myaddr, com_argv[i+1], 0);
			if (myaddr.host == INADDR_NONE)
				Sys_Error ("%s is not a valid IP address", com_argv[i+1]);
			strcpy(my_tcpip_address, com_argv[i+1]);
		}
		else
		{
			Sys_Error ("NET_Init: you must specify an IP address after -ip");
		}
	}
	else
	{
		/* with SDL_net, there is no way of doing an
		equivalent of gethostaddr() / gethostbyname(). */
		SDLNet_ResolveHost(&myaddr, NULL, 0);
		strcpy(my_tcpip_address, "INADDR_ANY");
	}

	// open the control socket
	if ((net_controlsocket = SDLN_OpenSocket(0)) == -1)
	{
		Con_Printf("SDLN_Init: Unable to open control socket\n");
		return -1;
	}

	broadcastaddr.qsa_family = AF_INET;
	ipaddress = (IPaddress *)&(broadcastaddr.qsa_data);
	SDLNet_Write32(INADDR_BROADCAST, &ipaddress->host);
	SDLNet_Write16(net_hostport, &ipaddress->port);

	Con_Printf("SDL_net TCP/IP initialized\n");
	tcpipAvailable = true;

	return net_controlsocket;
}

void SDLN_Shutdown (void)
{
	SDLN_Listen (false);
	SDLN_CloseSocket (net_controlsocket);
}

void SDLN_GetLocalAddress (void)
{
	if (myaddr.host != INADDR_ANY)
		return;

	SDLNet_ResolveHost(&myaddr, NULL, 0);
}

void SDLN_Listen (qboolean state)
{
	// enable listening
	if (state)
	{
		if (net_acceptsocket != -1)
			return;

		SDLN_GetLocalAddress();
		if ((net_acceptsocket = SDLN_OpenSocket (net_hostport)) == -1)
			Sys_Error ("SDLN_Listen: Unable to open accept socket\n");

		SDLNet_UDP_AddSocket(acceptsocket_set, net_sockets[net_acceptsocket]);
	}
	else
	{
	// disable listening
		if (net_acceptsocket == -1)
			return;

		SDLNet_UDP_DelSocket(acceptsocket_set, net_sockets[net_acceptsocket]);
		SDLN_CloseSocket(net_acceptsocket);

		net_acceptsocket = -1;
	}
}

int SDLN_OpenSocket (int port)
{
	UDPsocket		newsocket;
	static IPaddress	address;
	Uint16		_port = port;

	if ((newsocket = SDLNet_UDP_Open(_port)) == NULL)
		return -1;

	address.host = myaddr.host;
	address.port = SDLNet_Read16(&_port);

	if (SDLNet_UDP_Bind(newsocket, 0, &address) != -1)
		return socket_id(newsocket);

	Sys_Error ("Unable to bind to %s", _IPAddrToString(&address));

	SDLNet_UDP_Close(newsocket);
	return -1;
}

int SDLN_CloseSocket (int socketid)
{
	UDPsocket	socket_p;

	if (socketid == net_broadcastsocket)
		net_broadcastsocket = -1;

	socket_p = net_sockets[socketid];

	if (socket_p == NULL)
		return -1;

	SDLNet_UDP_Close(socket_p);

	net_sockets[socketid] = NULL;
	return 0;
}

int SDLN_Connect (int socketid, struct qsockaddr *addr)
{
	return 0;
}

int SDLN_CheckNewConnections (void)
{
	if (net_acceptsocket == -1)
		return -1;

	if (SDLNet_CheckSockets(acceptsocket_set, 0) > 0)
		return net_acceptsocket;

	return -1;
}

static UDPpacket *init_packet(UDPpacket *packet, int len)
{
	if (packet == NULL)
		return SDLNet_AllocPacket(len);

	if (packet->maxlen < len)
		SDLNet_ResizePacket(packet, len);

	return packet;
}

int SDLN_Read (int socketid, byte *buf, int len, struct qsockaddr *addr)
{
	int			numrecv;
	static UDPpacket	*packet;
	IPaddress		*ipaddress;
	UDPsocket		socket_p;

	socket_p = net_sockets[socketid];
	if (socket_p == NULL)
		return -1;

	packet = init_packet(packet, len);

	numrecv = SDLNet_UDP_Recv(socket_p, packet);
	if (numrecv == 1)
	{
		memcpy(buf, packet->data, packet->len);

		addr->qsa_family = AF_INET;
		ipaddress = (IPaddress *)&(addr->qsa_data);
		ipaddress->host = packet->address.host;
		ipaddress->port = packet->address.port;

		return packet->len;
	}

	return numrecv;
}

int SDLN_Write (int socketid, byte *buf, int len, struct qsockaddr *addr)
{
	int			numsent;
	static UDPpacket	*packet;
	UDPsocket		socket_p;
	IPaddress		*ipaddress;

	socket_p = net_sockets[socketid];
	if (socket_p == NULL)
		return -1;

	packet = init_packet(packet, len);
	memcpy(packet->data, buf, len);
	packet->len = len;

	ipaddress = (IPaddress *)&(addr->qsa_data);
	packet->address.host = ipaddress->host;
	packet->address.port = ipaddress->port;

	numsent = SDLNet_UDP_Send(socket_p, -1, packet);
	if (numsent == 0)
		return 0;

	return len;
}

int SDLN_Broadcast (int socketid, byte *buf, int len)
{
	if (socketid != net_broadcastsocket)
	{
		if (net_broadcastsocket != 0)
			Sys_Error("Attempted to use multiple broadcast sockets\n");

		// todo make socket broadcast capable
		Sys_Error("Unable to make socket broadcast capable\n");
	}

	return SDLN_Write(socketid, buf, len, &broadcastaddr);
}

const char *SDLN_AddrToString (struct qsockaddr *addr)
{
	int		ip;
	int		port;
	IPaddress	*ipaddress;

	ipaddress = (IPaddress *)&(addr->qsa_data);

	ip = SDLNet_Read32(&ipaddress->host);
	port = SDLNet_Read16(&ipaddress->port);

	return _AddrToString(ip, port);
}

int  SDLN_StringToAddr (const char *string, struct qsockaddr *addr)
{
	int ha1, ha2, ha3, ha4, hp;
	int hostaddr;
	IPaddress *ipaddress;

	sscanf(string, "%d.%d.%d.%d:%d", &ha1, &ha2, &ha3, &ha4, &hp);
	hostaddr = (ha1 << 24) | (ha2 << 16) | (ha3 << 8) | ha4;

	addr->qsa_family = AF_INET;
	ipaddress = (IPaddress *)&(addr->qsa_data);

	SDLNet_Write32(hostaddr, &ipaddress->host);
	SDLNet_Write16(hp, &ipaddress->port);

	return 0;
}

int SDLN_GetSocketAddr (int socketid, struct qsockaddr *addr)
{
	static UDPsocket	socket_p;
	IPaddress		*peeraddress;
	IPaddress		*ipaddress;

	Q_memset(addr, 0, sizeof(struct qsockaddr));

	socket_p = net_sockets[socketid];
	if (socket_p == NULL)
		return -1;

	peeraddress = SDLNet_UDP_GetPeerAddress(socket_p, -1);
	if (peeraddress == NULL)
		return -1;

	addr->qsa_family = AF_INET;
	ipaddress = (IPaddress *) addr->qsa_data;
	if (peeraddress->host == 0 ||
	    peeraddress->host == SDL_SwapBE32(INADDR_LOOPBACK) /* inet_addr ("127.0.0.1") */)
	{
		ipaddress->host = myaddr.host;
		ipaddress->port = myaddr.port;
	}
	else
	{
		ipaddress->host = peeraddress->host;
		ipaddress->port = peeraddress->port;
	}

	return 0;
}

int SDLN_GetNameFromAddr (struct qsockaddr *addr, char *name)
{
	const char	*buf;
	IPaddress	*ipaddress;

	ipaddress = (IPaddress *)&(addr->qsa_data);

	buf = SDLNet_ResolveIP(ipaddress);
	if (buf != NULL)
	{
		Q_strncpy(name, buf, NET_NAMELEN - 1);
		return 0;
	}

	Q_strcpy(name, SDLN_AddrToString(addr));
	return 0;
}

//=============================================================================
/*
============
PartialIPAddress

this lets you type only as much of the net address as required, using
the local network components to fill in the rest
============
*/
static int PartialIPAddress (const char *in, struct qsockaddr *hostaddr)
{
	char buff[256];
	char *b;
	int addr;
	int num;
	int mask;
	int tmp;
	int run;
	int port;
	IPaddress *ipaddress;

	buff[0] = '.';
	b = buff;
	strcpy(buff+1, in);
	if (buff[1] == '.')
		b++;

	addr = 0;
	mask=-1;
	while (*b == '.')
	{
		b++;
		num = 0;
		run = 0;
		while (!( *b < '0' || *b > '9'))
		{
			num = num*10 + *b++ - '0';
			if (++run > 3)
				return -1;
		}
		if ((*b < '0' || *b > '9') && *b != '.' && *b != ':' && *b != 0)
			return -1;
		if (num < 0 || num > 255)
			return -1;
		mask<<=8;
		addr = (addr<<8) + num;
	}

	if (*b++ == ':')
		port = Q_atoi(b);
	else
		port = net_hostport;

	tmp = SDLNet_Read32(&myaddr.host);
	tmp = (tmp & mask) | addr;

	hostaddr->qsa_family = AF_INET;
	ipaddress = (IPaddress *)&(hostaddr->qsa_data);

	SDLNet_Write32(tmp, &ipaddress->host);
	SDLNet_Write16(port, &ipaddress->port);

	return 0;
}

int SDLN_GetAddrFromName (const char *name, struct qsockaddr *addr)
{
	IPaddress   *ipaddress;

	if (name[0] >= '0' && name[0] <= '9')
		return PartialIPAddress (name, addr);

	ipaddress = (IPaddress *)&(addr->qsa_data);
	if (SDLNet_ResolveHost((IPaddress *)(&addr->qsa_data), name, net_hostport) == -1)
		return -1;

	addr->qsa_family = AF_INET;
	return 0;
}

int SDLN_AddrCompare (struct qsockaddr *addr1, struct qsockaddr *addr2)
{
	IPaddress *ipaddr1;
	IPaddress *ipaddr2;

	if (addr1->qsa_family != addr2->qsa_family)
		return -1;

	ipaddr1 = (IPaddress *)&(addr1->qsa_data);
	ipaddr2 = (IPaddress *)&(addr2->qsa_data);

	if (ipaddr1->host != ipaddr2->host)
		return -1;

	if (ipaddr1->port != ipaddr2->port)
		return 1;

	return 0;
}

int SDLN_GetSocketPort (struct qsockaddr *addr)
{
	IPaddress *ipaddress;

	ipaddress = (IPaddress *)&(addr->qsa_data);
	return SDLNet_Read16(&ipaddress->port);
}

int SDLN_SetSocketPort (struct qsockaddr *addr, int port)
{
	IPaddress *ipaddress;

	ipaddress = (IPaddress *)&(addr->qsa_data);
	SDLNet_Write16(port, &ipaddress->port);

	return 0;
}

