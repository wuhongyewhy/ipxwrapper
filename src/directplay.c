/* ipxwrapper - DirectPlay service provider
 * Copyright (C) 2011 Daniel Collins <solemnwarning@solemnwarning.net>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

/* TODO: ASYNC I/O!! */

#include <windows.h>
#include <dplaysp.h>
#include <winsock2.h>
#include <wsipx.h>

#include "ipxwrapper.h"

struct sp_data {
	SOCKET sock;
	struct sockaddr_ipx addr;
	
	struct sockaddr_ipx ns_addr;	/* sa_family is 0 when undefined */
	
	HANDLE worker_thread;
	DWORD worker_tid;
};

struct sp_data_cont {
	struct sp_data *data;
	HANDLE mutex;
};

const GUID IPX_GUID = {
	0x685BC400,
	0x9D2C,
	0x11CF,
	{0xA9, 0xCD, 0x00, 0xAA, 0x00, 0x68, 0x86, 0xE3}
};

#define DISCOVERY_SOCKET 42367
#define API_HEADER_SIZE sizeof(struct sockaddr_ipx)

/* Lock the object mutex and return the data pointer */
static struct sp_data *get_sp_data(IDirectPlaySP *sp) {
	struct sp_data_cont *cont;
	DWORD size;
	
	HRESULT r = IDirectPlaySP_GetSPData(sp, (void**)&cont, &size, DPGET_LOCAL);
	if(r != DP_OK) {
		log_printf("GetSPData: %d", (int)r);
		abort();
	}
	
	WaitForSingleObject(cont->mutex, INFINITE);
	
	return cont->data;
}

/* Release the object mutex */
static void release_sp_data(IDirectPlaySP *sp) {
	struct sp_data_cont *cont;
	DWORD size;
	
	HRESULT r = IDirectPlaySP_GetSPData(sp, (void**)&cont, &size, DPGET_LOCAL);
	if(r != DP_OK) {
		log_printf("GetSPData: %d", (int)r);
		abort();
	}
	
	ReleaseMutex(cont->mutex);
}

static DWORD WINAPI worker_main(LPVOID arg) {
	struct sp_data *sp_data = get_sp_data((IDirectPlaySP*)arg);
	
	int sockfd = sp_data->sock;
	
	release_sp_data((IDirectPlaySP*)arg);
	
	char *buf = malloc(PACKET_BUF_SIZE);
	if(!buf) {
		abort();
	}
	
	while(1) {
		struct sockaddr_ipx addr;
		int addrlen = sizeof(addr);
		
		int r = recvfrom(sockfd, buf, PACKET_BUF_SIZE, 0, (struct sockaddr*)&addr, &addrlen);
		if(r == -1) {
			log_printf("recv failed");
			return 0;
		}
		
		HRESULT h = IDirectPlaySP_HandleMessage((IDirectPlaySP*)arg, buf, r, &addr);
		if(h != DP_OK) {
			log_printf("HandleMessage error: %d", (int)h);
		}
	}
	
	return 0;
}

static BOOL init_worker(IDirectPlaySP *sp) {
	struct sp_data *sp_data = get_sp_data(sp);
	
	if(sp_data->worker_thread) {
		release_sp_data(sp);
		return TRUE;
	}
	
	sp_data->worker_thread = CreateThread(NULL, 0, &worker_main, sp, 0, &(sp_data->worker_tid));
	if(!sp_data->worker_thread) {
		log_printf("Failed to create worker thread");
		
		release_sp_data(sp);
		return FALSE;
	}
	
	release_sp_data(sp);
	return TRUE;
}

static HRESULT WINAPI IPX_EnumSessions(LPDPSP_ENUMSESSIONSDATA data) {
	//log_printf("IPX_EnumSessions called");
	
	if(!init_worker(data->lpISP)) {
		return DPERR_GENERIC;
	}
	
	struct sp_data *sp_data = get_sp_data(data->lpISP);
	
	struct sockaddr_ipx addr;
	
	memcpy(&addr, &(sp_data->addr), sizeof(addr));
	
	memset(addr.sa_nodenum, 0xFF, 6);
	addr.sa_socket = htons(DISCOVERY_SOCKET);
	
	if(sendto(sp_data->sock, data->lpMessage + API_HEADER_SIZE, data->dwMessageSize - API_HEADER_SIZE, 0, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
		log_printf("sendto failed: %s", w32_error(WSAGetLastError()));
		
		release_sp_data(data->lpISP);
		return DPERR_GENERIC;
	}
	
	release_sp_data(data->lpISP);
	return DP_OK;
}

static HRESULT WINAPI IPX_Send(LPDPSP_SENDDATA data) {
	//log_printf("IPX_Send called");
	
	struct sockaddr_ipx *addr = NULL;
	
	struct sp_data *sp_data = get_sp_data(data->lpISP);
	
	if(data->idPlayerTo) {
		DWORD size;
		
		HRESULT r = IDirectPlaySP_GetSPPlayerData(data->lpISP, data->idPlayerTo, (void**)&addr, &size, DPGET_LOCAL);
		if(r != DP_OK) {
			addr = NULL;
			//log_printf("GetSPPlayerData: %d", r);
		}
	}else if(sp_data->ns_addr.sa_family) {
		addr = &(sp_data->ns_addr);
	}
	
	if(!addr) {
		log_printf("No known address for player ID %u, dropping packet", data->idPlayerTo);
		
		release_sp_data(data->lpISP);
		return DP_OK;
	}
	
	if(sendto(sp_data->sock, data->lpMessage + API_HEADER_SIZE, data->dwMessageSize - API_HEADER_SIZE, 0, (struct sockaddr*)addr, sizeof(*addr)) == -1) {
		log_printf("sendto failed: %s", w32_error(WSAGetLastError()));
		
		release_sp_data(data->lpISP);
		return DPERR_GENERIC;
	}
	
	release_sp_data(data->lpISP);
	return DP_OK;
}

static HRESULT WINAPI IPX_SendEx(LPDPSP_SENDEXDATA data) {
	//log_printf("IPX_SendEx called");
	
	DPSP_SENDDATA s_data;
	
	char *buf = malloc(data->dwMessageSize);
	size_t off = 0, i;
	
	for(i = 0; i < data->cBuffers; i++) {
		if(off + data->lpSendBuffers[i].len > data->dwMessageSize) {
			log_printf("dwMessageSize too small, aborting");
			return DPERR_GENERIC;
		}
		
		memcpy(buf + off, data->lpSendBuffers[i].pData, data->lpSendBuffers[i].len);
		off += data->lpSendBuffers[i].len;
	}
	
	s_data.dwFlags = data->dwFlags;
	s_data.idPlayerTo = data->idPlayerTo;
	s_data.idPlayerFrom = data->idPlayerFrom;
	s_data.lpMessage = buf;
	s_data.dwMessageSize = data->dwMessageSize;
	s_data.bSystemMessage = data->bSystemMessage;
	s_data.lpISP = data->lpISP;
	
	HRESULT ret = IPX_Send(&s_data);
	
	free(buf);
	
	return ret;
}

static HRESULT WINAPI IPX_Reply(LPDPSP_REPLYDATA data) {
	//log_printf("IPX_Reply called (idNameServer = %u)", data->idNameServer);
	
	/* TODO: Only update ns_addr if idNameServer has changed? */
	
	struct sockaddr_ipx *addr_p;
	struct sp_data *sp_data = get_sp_data(data->lpISP);
	DWORD size;
	
	HRESULT r = IDirectPlaySP_GetSPPlayerData(data->lpISP, data->idNameServer, (void**)&addr_p, &size, DPGET_LOCAL);
	//log_printf("GetSPPlayerData: %d", r);
	
	if(r == DP_OK && addr_p) {
		memcpy(&(sp_data->ns_addr), addr_p, sizeof(struct sockaddr_ipx));
	}
	
	/* Do the actual sending */
	
	if(sendto(sp_data->sock, data->lpMessage + API_HEADER_SIZE, data->dwMessageSize - API_HEADER_SIZE, 0, (struct sockaddr*)data->lpSPMessageHeader, sizeof(struct sockaddr_ipx)) == -1) {
		log_printf("sendto failed: %s", w32_error(WSAGetLastError()));
		
		release_sp_data(data->lpISP);
		return DPERR_GENERIC;
	}
	
	release_sp_data(data->lpISP);
	return DP_OK;
}

static HRESULT WINAPI IPX_CreatePlayer(LPDPSP_CREATEPLAYERDATA data) {
	if(data->lpSPMessageHeader) {
		HRESULT r = IDirectPlaySP_SetSPPlayerData(data->lpISP, data->idPlayer, data->lpSPMessageHeader, sizeof(struct sockaddr_ipx), DPSET_LOCAL);
		if(r != DP_OK) {
			log_printf("SetSPPlayerData: %d", (int)r);
			return DPERR_GENERIC;
		}
	}
	
	return DP_OK;
}

static HRESULT WINAPI IPX_DeletePlayer(LPDPSP_DELETEPLAYERDATA data) {
	return DP_OK;
}

static HRESULT WINAPI IPX_GetCaps(LPDPSP_GETCAPSDATA data) {
	//log_printf("IPX_GetCaps called");
	
	if(data->lpCaps->dwSize < sizeof(DPCAPS)) {
		/* It's either this or DPERR_INVALIDOBJECT according to DirectX 7.0 */
		return DPERR_INVALIDPARAMS;
	}
	
	/* Most values are incorrect/inaccurate, copied from the MS implementation
	 * for compatibility.
	*/
	
	data->lpCaps->dwFlags = 0;
	data->lpCaps->dwMaxBufferSize = 1024;
	data->lpCaps->dwMaxQueueSize = 0;
	data->lpCaps->dwMaxPlayers = 65536;
	data->lpCaps->dwHundredBaud = 0;
	data->lpCaps->dwLatency = 50;
	data->lpCaps->dwMaxLocalPlayers = 65536;
	data->lpCaps->dwHeaderLength = API_HEADER_SIZE;
	data->lpCaps->dwTimeout = 500;
	
	return DP_OK;
}

static HRESULT WINAPI IPX_Open(LPDPSP_OPENDATA data) {
	//log_printf("IPX_Open called");
	
	if(!init_worker(data->lpISP)) {
		return DPERR_GENERIC;
	}
	
	struct sp_data *sp_data = get_sp_data(data->lpISP);
	
	if(data->bCreate) {
		struct sockaddr_ipx addr;
		
		memcpy(&addr, &(sp_data->addr), sizeof(addr));
		addr.sa_socket = htons(DISCOVERY_SOCKET);
	
		if(ipx_ex_bind(sp_data->sock, &addr) == -1) {
			release_sp_data(data->lpISP);
			return DPERR_CANNOTCREATESERVER;
		}
	}else if(data->lpSPMessageHeader) {
		memcpy(&(sp_data->ns_addr), data->lpSPMessageHeader, sizeof(struct sockaddr_ipx));
	}
	
	release_sp_data(data->lpISP);
	return DP_OK;
}

static HRESULT WINAPI IPX_CloseEx(LPDPSP_CLOSEDATA data) {
	struct sp_data *sp_data = get_sp_data(data->lpISP);
	
	/* Disable the special bind if in use */
	ipx_ex_bind(sp_data->sock, NULL);
	
	release_sp_data(data->lpISP);
	return DP_OK;
}

static HRESULT WINAPI IPX_ShutdownEx(LPDPSP_SHUTDOWNDATA data) {
	struct sp_data *sp_data = get_sp_data(data->lpISP);
	
	if(sp_data->worker_thread && GetCurrentThreadId() != sp_data->worker_tid) {
		TerminateThread(sp_data->worker_thread, 0);
		sp_data->worker_thread = NULL;
	}
	
	closesocket(sp_data->sock);
	
	release_sp_data(data->lpISP);
	return DP_OK;
}

HRESULT WINAPI r_SPInit(LPSPINITDATA);

HRESULT WINAPI SPInit(LPSPINITDATA data) {
	if(!IsEqualGUID(data->lpGuid, &IPX_GUID)) {
		return r_SPInit(data);
	}
	
	log_printf("SPInit: %p", data->lpISP);
	
	{
		struct sp_data_cont *cont;
		DWORD size;
		
		HRESULT r = IDirectPlaySP_GetSPData(data->lpISP, (void**)&cont, &size, DPGET_LOCAL);
		if(r != DP_OK) {
			log_printf("SPInit: GetSPData: %d", r);
			return DPERR_UNAVAILABLE;
		}
		
		if(cont) {
			log_printf("SPInit: Already initialised, returning DP_OK");
			return DP_OK;
		}
	}
	
	struct sp_data *sp_data = malloc(sizeof(struct sp_data));
	if(!sp_data) {
		return DPERR_UNAVAILABLE;
	}
	
	HANDLE mutex = CreateMutex(NULL, FALSE, NULL);
	if(!mutex) {
		free(sp_data);
		return DPERR_UNAVAILABLE;
	}
	
	if((sp_data->sock = socket(AF_IPX, SOCK_DGRAM, NSPROTO_IPX)) == -1) {
		CloseHandle(mutex);
		free(sp_data);
		
		return DPERR_UNAVAILABLE;
	}
	
	struct sockaddr_ipx addr;
	memset(&addr, 0, sizeof(addr));
	addr.sa_family = AF_IPX;
	addr.sa_socket = 0;
	
	if(bind(sp_data->sock, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
		closesocket(sp_data->sock);
		CloseHandle(mutex);
		free(sp_data);
		
		return DPERR_UNAVAILABLE;
	}
	
	int addrlen = sizeof(sp_data->addr);
	
	if(getsockname(sp_data->sock, (struct sockaddr*)&(sp_data->addr), &addrlen) == -1) {
		log_printf("getsockname failed: %s", w32_error(WSAGetLastError()));
		
		closesocket(sp_data->sock);
		CloseHandle(mutex);
		free(sp_data);
		
		return DPERR_UNAVAILABLE;
	}
	
	sp_data->ns_addr.sa_family = 0;
	sp_data->worker_thread = NULL;
	
	BOOL bcast = TRUE;
	setsockopt(sp_data->sock, SOL_SOCKET, SO_BROADCAST, (char*)&bcast, sizeof(BOOL));
	
	struct sp_data_cont cont;
	cont.data = sp_data;
	cont.mutex = mutex;
	
	HRESULT r = IDirectPlaySP_SetSPData(data->lpISP, &cont, sizeof(cont), DPSET_LOCAL);
	if(r != DP_OK) {
		log_printf("SetSPData: %d", (int)r);
		
		closesocket(sp_data->sock);
		CloseHandle(mutex);
		free(sp_data);
		
		return DPERR_UNAVAILABLE;
	}
	
	data->lpCB->EnumSessions = &IPX_EnumSessions;
	data->lpCB->Send = &IPX_Send;
	data->lpCB->SendEx = &IPX_SendEx;
	data->lpCB->Reply = &IPX_Reply;
	data->lpCB->CreatePlayer = &IPX_CreatePlayer;
	data->lpCB->DeletePlayer = &IPX_DeletePlayer;
	data->lpCB->GetCaps = &IPX_GetCaps;
	data->lpCB->Open = &IPX_Open;
	data->lpCB->CloseEx = &IPX_CloseEx;
	data->lpCB->ShutdownEx = &IPX_ShutdownEx;
	
	data->dwSPHeaderSize = API_HEADER_SIZE;
	data->dwSPVersion = DPSP_MAJORVERSIONMASK & DPSP_MAJORVERSION;
	
	return DP_OK;
}

/* Convert a windows error number to an error message */
char const *w32_error(DWORD errnum) {
	static char buf[1024] = {'\0'};
	
	FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, NULL, errnum, 0, buf, 1023, NULL);
	buf[strcspn(buf, "\r\n")] = '\0';
	return buf;	
}

extern char const *dllname;
unsigned char log_calls = 0;
static HMODULE sysdll = NULL;

BOOL WINAPI DllMain(HINSTANCE me, DWORD why, LPVOID res) {
	if(why == DLL_PROCESS_ATTACH) {
		log_open();
		
		HKEY key;
		
		if(RegOpenKeyEx(HKEY_CURRENT_USER, "Software\\IPXWrapper", 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
			DWORD size = 1;
			
			if(RegQueryValueEx(key, "log_calls", NULL, NULL, (BYTE*)&log_calls, &size) != ERROR_SUCCESS || size != 1) {
				log_calls = 0;
			}
			
			RegCloseKey(key);
		}
	}else if(why == DLL_PROCESS_DETACH) {
		if(sysdll) {
			FreeLibrary(sysdll);
			sysdll = NULL;
		}
		
		log_close();
	}
	
	return TRUE;
}

void __stdcall *find_sym(char const *symbol) {
	if(!sysdll) {
		char sysdir[1024], path[1024];
	
		GetSystemDirectory(sysdir, 1024);
		snprintf(path, 1024, "%s\\%s", sysdir, dllname);
		
		if(!(sysdll = LoadLibrary(path))) {
			abort();
		}
	}
	
	void *ptr = GetProcAddress(sysdll, symbol);
	
	if(!ptr) {
		log_printf("Missing symbol in %s: %s", dllname, symbol);
		abort();
	}
	
	return ptr;
}
