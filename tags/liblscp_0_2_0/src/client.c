// client.c
//
/****************************************************************************
   liblscp - LinuxSampler Control Protocol API
   Copyright (C) 2004, rncbc aka Rui Nuno Capela. All rights reserved.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

*****************************************************************************/

#include "common.h"

// Default timeout value (in milliseconds).
#define LSCP_TIMEOUT_MSECS  500


// Local prototypes.

static void             _lscp_client_evt_proc       (void *pvClient);

static lscp_status_t    _lscp_client_evt_connect    (lscp_client_t *pClient);
static lscp_status_t    _lscp_client_evt_request    (lscp_client_t *pClient, int iSubscribe, lscp_event_t event);


//-------------------------------------------------------------------------
// Event service (datagram oriented).

static void _lscp_client_evt_proc ( void *pvClient )
{
    lscp_client_t *pClient = (lscp_client_t *) pvClient;
    
    fd_set fds;                         // File descriptor list for select().
    int    fd, fdmax;                   // Maximum file descriptor number.
    struct timeval tv;                  // For specifying a timeout value.
    int    iSelect;                     // Holds select return status.
    int    iTimeout;
    
    char   achBuffer[LSCP_BUFSIZ];
    int    cchBuffer;
    const char *pszSeps = ":\r\n";
    char * pszToken;
    char * pch;
    int     cchToken;
    lscp_event_t event;

#ifdef DEBUG
    fprintf(stderr, "_lscp_client_evt_proc: Client waiting for events.\n");
#endif

    while (pClient->evt.iState) {

        // Prepare for waiting on select...
        fd = (int) pClient->evt.sock;
        FD_ZERO(&fds);
        FD_SET((unsigned int) fd, &fds);
        fdmax = fd;

        // Use the timeout (x10) select feature ...
        iTimeout = 10 * pClient->iTimeout;
        if (iTimeout > 1000) {
            tv.tv_sec = iTimeout / 1000;
            iTimeout -= tv.tv_sec * 1000;
        }
        else tv.tv_sec = 0;
        tv.tv_usec = iTimeout * 1000;

        // Wait for event...
        iSelect = select(fdmax + 1, &fds, NULL, NULL, &tv);
        if (iSelect > 0 && FD_ISSET(fd, &fds)) {
            // May recv now...
            cchBuffer = recv(pClient->evt.sock, achBuffer, sizeof(achBuffer), 0);
            if (cchBuffer > 0) {
                // Make sure received buffer it's null terminated.
                achBuffer[cchBuffer] = (char) 0;
                // Parse for the notification event message...
                pszToken = lscp_strtok(achBuffer, pszSeps, &(pch)); // Have "NOTIFY".
                if (strcasecmp(pszToken, "NOTIFY") == 0) {
                    pszToken = lscp_strtok(NULL, pszSeps, &(pch));
                    event    = lscp_event_from_text(pszToken);
                    // And pick the rest of data...
                    pszToken = lscp_strtok(NULL, pszSeps, &(pch));
                    cchToken = (pszToken == NULL ? 0 : strlen(pszToken));
                    // Double-check if we're really up to it...
                    if (pClient->events & event) {
                        // Invoke the client event callback...
                        if ((*pClient->pfnCallback)(
                                pClient,
                                event,
                                pszToken,
                                cchToken,
                                pClient->pvData) != LSCP_OK) {
                            pClient->evt.iState = 0;
                        }
                    }
                }
            } else {
                lscp_socket_perror("_lscp_client_evt_proc: recv");
                pClient->evt.iState = 0;
            }
        }   // Check if select has in error.
        else if (iSelect < 0) {
            lscp_socket_perror("_lscp_client_call: select");
            pClient->evt.iState = 0;
        }
        
        // Finally, always signal the event.
        lscp_cond_signal(pClient->cond);
    }

#ifdef DEBUG
    fprintf(stderr, "_lscp_client_evt_proc: Client closing.\n");
#endif
}


//-------------------------------------------------------------------------
// Event subscription helpers.

// Open the event service socket connection.
static lscp_status_t _lscp_client_evt_connect ( lscp_client_t *pClient )
{
    lscp_socket_t sock;
    struct sockaddr_in addr;
    int cAddr;
#if defined(WIN32)
    int iSockOpt = (-1);
#endif

    // Prepare the event connection socket...
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        lscp_socket_perror("_lscp_client_evt_connect: socket");
        return LSCP_FAILED;
    }

#if defined(WIN32)
    if (setsockopt(sock, SOL_SOCKET, SO_DONTLINGER, (char *) &iSockOpt, sizeof(int)) == SOCKET_ERROR)
        lscp_socket_perror("lscp_client_evt_connect: setsockopt(SO_DONTLINGER)");
#endif

#ifdef DEBUG
    lscp_socket_getopts("_lscp_client_evt_connect:", sock);
#endif

    // Use same address of the command connection.
    cAddr = sizeof(struct sockaddr_in);
    memmove((char *) &addr, &(pClient->cmd.addr), cAddr);

    // Start the connection...
    if (connect(sock, (struct sockaddr *) &addr, cAddr) == SOCKET_ERROR) {
        lscp_socket_perror("_lscp_client_evt_connect: connect");
        closesocket(sock);
        return LSCP_FAILED;
    }
    
    // Set our socket agent struct...
    lscp_socket_agent_init(&(pClient->evt), sock, &addr, cAddr);
    
    // And finally the service thread...
    return lscp_socket_agent_start(&(pClient->evt), _lscp_client_evt_proc, pClient, 0);
}


// Subscribe to a single event.
static lscp_status_t _lscp_client_evt_request ( lscp_client_t *pClient, int iSubscribe, lscp_event_t event )
{
    const char *pszEvent;
    char  szQuery[LSCP_BUFSIZ];
    int   cchQuery;

    if (pClient == NULL)
        return LSCP_FAILED;

    // Which (single) event?
    pszEvent = lscp_event_to_text(event);
    if (pszEvent == NULL)
        return LSCP_FAILED;

    // Build the query string...
    cchQuery = sprintf(szQuery, "%sSUBSCRIBE %s\n\n", (iSubscribe == 0 ? "UN" : ""), pszEvent);
    // Just send data, forget result...
    if (send(pClient->evt.sock, szQuery, cchQuery, 0) < cchQuery) {
        lscp_socket_perror("_lscp_client_evt_request: send");
        return LSCP_FAILED;
    }

    // Wait on response.
    lscp_cond_wait(pClient->cond, pClient->mutex);
    
    // Update as naively as we can...
    if (iSubscribe)
        pClient->events |=  event;
    else
        pClient->events &= ~event;

    return LSCP_OK;
}


//-------------------------------------------------------------------------
// Client versioning teller fuunction.


/** Retrieve the current client library version string. */
const char* lscp_client_package (void) { return LSCP_PACKAGE; }

/** Retrieve the current client library version string. */
const char* lscp_client_version (void) { return LSCP_VERSION; }

/** Retrieve the current client library build timestamp string. */
const char* lscp_client_build   (void) { return __DATE__ " " __TIME__; }


//-------------------------------------------------------------------------
// Client socket functions.

/**
 *  Create a client instance, estabilishing a connection to a server hostname,
 *  which must be listening on the given port. A client callback function is
 *  also supplied for server notification event handling.
 *
 *  @param pszHost      Hostname of the linuxsampler listening server.
 *  @param iPort        Port number of the linuxsampler listening server.
 *  @param pfnCallback  Callback function to receive event notifications.
 *  @param pvData       User context opaque data, that will be passed
 *                      to the callback function.
 *
 *  @returns The new client instance pointer if successfull, which shall be
 *  used on all subsequent client calls, NULL otherwise.
 */
lscp_client_t* lscp_client_create ( const char *pszHost, int iPort, lscp_client_proc_t pfnCallback, void *pvData )
{
    lscp_client_t  *pClient;
    struct hostent *pHost;
    lscp_socket_t sock;
    struct sockaddr_in addr;
    int cAddr;
#if defined(WIN32)
    int iSockOpt = (-1);
#endif

    if (pfnCallback == NULL) {
        fprintf(stderr, "lscp_client_create: Invalid client callback function.\n");
        return NULL;
    }

    pHost = gethostbyname(pszHost);
    if (pHost == NULL) {
        lscp_socket_herror("lscp_client_create: gethostbyname");
        return NULL;
    }

    // Allocate client descriptor...

    pClient = (lscp_client_t *) malloc(sizeof(lscp_client_t));
    if (pClient == NULL) {
        fprintf(stderr, "lscp_client_create: Out of memory.\n");
        return NULL;
    }
    memset(pClient, 0, sizeof(lscp_client_t));

    pClient->pfnCallback = pfnCallback;
    pClient->pvData = pvData;

#ifdef DEBUG
    fprintf(stderr, "lscp_client_create: pClient=%p: pszHost=%s iPort=%d.\n", pClient, pszHost, iPort);
#endif

    // Prepare the command connection socket...

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        lscp_socket_perror("lscp_client_create: cmd: socket");
        free(pClient);
        return NULL;
    }

#if defined(WIN32)
    if (setsockopt(sock, SOL_SOCKET, SO_DONTLINGER, (char *) &iSockOpt, sizeof(int)) == SOCKET_ERROR)
        lscp_socket_perror("lscp_client_create: cmd: setsockopt(SO_DONTLINGER)");
#endif

#ifdef DEBUG
    lscp_socket_getopts("lscp_client_create: cmd", sock);
#endif

    cAddr = sizeof(struct sockaddr_in);
    memset((char *) &addr, 0, cAddr);
    addr.sin_family = pHost->h_addrtype;
    memmove((char *) &(addr.sin_addr), pHost->h_addr, pHost->h_length);
    addr.sin_port = htons((short) iPort);

    if (connect(sock, (struct sockaddr *) &addr, cAddr) == SOCKET_ERROR) {
        lscp_socket_perror("lscp_client_create: cmd: connect");
        closesocket(sock);
        free(pClient);
        return NULL;
    }

    // Initialize the command socket agent struct...
    lscp_socket_agent_init(&(pClient->cmd), sock, &addr, cAddr);

#ifdef DEBUG
    fprintf(stderr, "lscp_client_create: cmd: pClient=%p: sock=%d addr=%s port=%d.\n", pClient, pClient->cmd.sock, inet_ntoa(pClient->cmd.addr.sin_addr), ntohs(pClient->cmd.addr.sin_port));
#endif

    // Initialize the event service socket struct...
    lscp_socket_agent_init(&(pClient->evt), INVALID_SOCKET, NULL, 0);
    // No events subscribed, yet.
    pClient->events = LSCP_EVENT_NONE;
    // Initialize cached members.
    pClient->audio_drivers = NULL;
    pClient->midi_drivers = NULL;
    pClient->audio_devices = NULL;
    pClient->midi_devices = NULL;
    pClient->engines = NULL;
    pClient->channels = NULL;
    lscp_driver_info_init(&(pClient->audio_driver_info));
    lscp_driver_info_init(&(pClient->midi_driver_info));
    lscp_device_info_init(&(pClient->audio_device_info));
    lscp_device_info_init(&(pClient->midi_device_info));
    lscp_param_info_init(&(pClient->audio_param_info));
    lscp_param_info_init(&(pClient->midi_param_info));
    lscp_device_port_info_init(&(pClient->audio_channel_info));
    lscp_device_port_info_init(&(pClient->midi_port_info));
    lscp_param_info_init(&(pClient->audio_channel_param_info));
    lscp_param_info_init(&(pClient->midi_port_param_info));
    lscp_engine_info_init(&(pClient->engine_info));
    lscp_channel_info_init(&(pClient->channel_info));
    // Initialize error stuff.
    pClient->pszResult = NULL;
    pClient->iErrno = -1;
    // Stream usage stuff.
    pClient->buffer_fill = NULL;
    pClient->iStreamCount = 0;
    // Default timeout value.
    pClient->iTimeout = LSCP_TIMEOUT_MSECS;

    // Initialize the transaction mutex.
    lscp_mutex_init(pClient->mutex);
    lscp_cond_init(pClient->cond);

    // Finally we've some success...
    return pClient;
}


/**
 *  Wait for a client instance to terminate graciously.
 *
 *  @param pClient  Pointer to client instance structure.
 */
lscp_status_t lscp_client_join ( lscp_client_t *pClient )
{
    if (pClient == NULL)
        return LSCP_FAILED;

#ifdef DEBUG
    fprintf(stderr, "lscp_client_join: pClient=%p.\n", pClient);
#endif

//  lscp_socket_agent_join(&(pClient->evt));
    lscp_socket_agent_join(&(pClient->cmd));

    return LSCP_OK;
}


/**
 *  Terminate and destroy a client instance.
 *
 *  @param pClient  Pointer to client instance structure.
 *
 *  @returns LSCP_OK on success, LSCP_FAILED otherwise.
 */
lscp_status_t lscp_client_destroy ( lscp_client_t *pClient )
{
    if (pClient == NULL)
        return LSCP_FAILED;

#ifdef DEBUG
    fprintf(stderr, "lscp_client_destroy: pClient=%p.\n", pClient);
#endif

    // Lock this section up.
    lscp_mutex_lock(pClient->mutex);
    
    // Free up all cached members.
    lscp_channel_info_free(&(pClient->channel_info));
    lscp_engine_info_free(&(pClient->engine_info));
    lscp_param_info_free(&(pClient->midi_port_param_info));
    lscp_param_info_free(&(pClient->audio_channel_param_info));
    lscp_device_port_info_free(&(pClient->midi_port_info));
    lscp_device_port_info_free(&(pClient->audio_channel_info));
    lscp_param_info_free(&(pClient->midi_param_info));
    lscp_param_info_free(&(pClient->audio_param_info));
    lscp_device_info_free(&(pClient->midi_device_info));
    lscp_device_info_free(&(pClient->audio_device_info));
    lscp_driver_info_free(&(pClient->midi_driver_info));
    lscp_driver_info_free(&(pClient->audio_driver_info));
    // Free available engine table.
    lscp_szsplit_destroy(pClient->audio_drivers);
    lscp_szsplit_destroy(pClient->midi_drivers);
    lscp_isplit_destroy(pClient->audio_devices);
    lscp_isplit_destroy(pClient->midi_devices);
    lscp_szsplit_destroy(pClient->engines);
    lscp_isplit_destroy(pClient->channels);
    // Make them null.
    pClient->audio_drivers = NULL;
    pClient->midi_drivers = NULL;
    pClient->engines = NULL;
    // Free result error stuff.
    lscp_client_set_result(pClient, NULL, 0);
    // Free stream usage stuff.
    if (pClient->buffer_fill)
        free(pClient->buffer_fill);
    pClient->buffer_fill = NULL;
    pClient->iStreamCount = 0;
    pClient->iTimeout = 0;

    // Free socket agents.
    lscp_socket_agent_free(&(pClient->evt));
    lscp_socket_agent_free(&(pClient->cmd));

    // Last but not least, free good ol'transaction mutex.
    lscp_mutex_unlock(pClient->mutex);
    lscp_mutex_destroy(pClient->mutex);
    lscp_cond_destroy(pClient->cond);

    free(pClient);

    return LSCP_OK;
}


/**
 *  Set the client transaction timeout interval.
 *
 *  @param pClient  Pointer to client instance structure.
 *  @param iTimeout Transaction timeout in milliseconds.
 *
 *  @returns LSCP_OK on success, LSCP_FAILED otherwise.
 */
lscp_status_t lscp_client_set_timeout ( lscp_client_t *pClient, int iTimeout )
{
    if (pClient == NULL)
        return LSCP_FAILED;
    if (iTimeout < 0)
        return LSCP_FAILED;

    pClient->iTimeout = iTimeout;
    return LSCP_OK;
}


/**
 *  Get the client transaction timeout interval.
 *
 *  @param pClient  Pointer to client instance structure.
 *
 *  @returns The current timeout value milliseconds, -1 in case of failure.
 */
int lscp_client_get_timeout ( lscp_client_t *pClient )
{
    if (pClient == NULL)
        return -1;

    return pClient->iTimeout;
}


//-------------------------------------------------------------------------
// Client common protocol functions.

/**
 *  Submit a command query line string to the server. The query string
 *  must be cr/lf and null terminated. Besides the return code, the
 *  specific server response to the command request is made available
 *  by the @ref lscp_client_get_result and @ref lscp_client_get_errno
 *  function calls.
 *
 *  @param pClient      Pointer to client instance structure.
 *  @param pszQuery     Command request line to be sent to server,
 *                      must be cr/lf and null terminated.
 *
 *  @returns LSCP_OK on success, LSCP_FAILED otherwise.
 */
lscp_status_t lscp_client_query ( lscp_client_t *pClient, const char *pszQuery )
{
    lscp_status_t ret;
    
    // Lock this section up.
    lscp_mutex_lock(pClient->mutex);

    // Just make the now guarded call.
    ret = lscp_client_call(pClient, pszQuery);
    
    // Unlock this section down.
    lscp_mutex_unlock(pClient->mutex);
    
    return ret;
}

/**
 *  Get the last received result string. In case of error or warning,
 *  this is the text of the error or warning message issued.
 *
 *  @param pClient  Pointer to client instance structure.
 *
 *  @returns A pointer to the literal null-terminated result string as
 *  of the last command request.
 */
const char *lscp_client_get_result ( lscp_client_t *pClient )
{
    if (pClient == NULL)
        return NULL;

    return pClient->pszResult;
}


/**
 *  Get the last error/warning number received.
 *
 *  @param pClient  Pointer to client instance structure.
 *
 *  @returns The numerical value of the last error or warning
 *  response code received.
 */
int lscp_client_get_errno ( lscp_client_t *pClient )
{
    if (pClient == NULL)
        return -1;

    return pClient->iErrno;
}


//-------------------------------------------------------------------------
// Client registration protocol functions.

/**
 *  Register frontend for receiving event messages:
 *  SUBSCRIBE CHANNELS | VOICE_COUNT | STREAM_COUNT | BUFFER_FILL
 *      | CHANNEL_INFO | MISCELLANEOUS
 *
 *  @param pClient  Pointer to client instance structure.
 *  @param events   Bit-wise OR'ed event flags to subscribe.
 *
 *  @returns LSCP_OK on success, LSCP_FAILED otherwise.
 */
lscp_status_t lscp_client_subscribe ( lscp_client_t *pClient, lscp_event_t events )
{
    lscp_status_t ret = LSCP_FAILED;

    if (pClient == NULL)
        return LSCP_FAILED;

    // Lock this section up.
    lscp_mutex_lock(pClient->mutex);

    // If applicable, start the alternate connection...
    if (pClient->events == LSCP_EVENT_NONE)
        ret = _lscp_client_evt_connect(pClient);
    
    // Send the subscription commands.
    if (ret == LSCP_OK && (events & LSCP_EVENT_CHANNELS))
        ret = _lscp_client_evt_request(pClient, 1, LSCP_EVENT_CHANNELS);
    if (ret == LSCP_OK && (events & LSCP_EVENT_VOICE_COUNT))
        ret = _lscp_client_evt_request(pClient, 1, LSCP_EVENT_VOICE_COUNT);
    if (ret == LSCP_OK && (events & LSCP_EVENT_STREAM_COUNT))
        ret = _lscp_client_evt_request(pClient, 1, LSCP_EVENT_STREAM_COUNT);
    if (ret == LSCP_OK && (events & LSCP_EVENT_BUFFER_FILL))
        ret = _lscp_client_evt_request(pClient, 1, LSCP_EVENT_BUFFER_FILL);
    if (ret == LSCP_OK && (events & LSCP_EVENT_CHANNEL_INFO))
        ret = _lscp_client_evt_request(pClient, 1, LSCP_EVENT_CHANNEL_INFO);
    if (ret == LSCP_OK && (events & LSCP_EVENT_MISCELLANEOUS))
        ret = _lscp_client_evt_request(pClient, 1, LSCP_EVENT_MISCELLANEOUS);

    // Unlock this section down.
    lscp_mutex_unlock(pClient->mutex);

    return ret;
}


/**
 *  Deregister frontend from receiving UDP event messages anymore:
 *  SUBSCRIBE CHANNELS | VOICE_COUNT | STREAM_COUNT | BUFFER_FILL
 *      | CHANNEL_INFO | MISCELLANEOUS
 *
 *  @param pClient  Pointer to client instance structure.
 *  @param events   Bit-wise OR'ed event flags to unsubscribe.
 *
 *  @returns LSCP_OK on success, LSCP_FAILED otherwise.
 */
lscp_status_t lscp_client_unsubscribe ( lscp_client_t *pClient, lscp_event_t events )
{
    lscp_status_t ret = LSCP_OK;

    if (pClient == NULL)
        return LSCP_FAILED;

    // Lock this section up.
    lscp_mutex_lock(pClient->mutex);

    // Send the unsubscription commands.
    if (ret == LSCP_OK && (events & LSCP_EVENT_CHANNELS))
        ret = _lscp_client_evt_request(pClient, 0, LSCP_EVENT_CHANNELS);
    if (ret == LSCP_OK && (events & LSCP_EVENT_VOICE_COUNT))
        ret = _lscp_client_evt_request(pClient, 0, LSCP_EVENT_VOICE_COUNT);
    if (ret == LSCP_OK && (events & LSCP_EVENT_STREAM_COUNT))
        ret = _lscp_client_evt_request(pClient, 0, LSCP_EVENT_STREAM_COUNT);
    if (ret == LSCP_OK && (events & LSCP_EVENT_BUFFER_FILL))
        ret = _lscp_client_evt_request(pClient, 0, LSCP_EVENT_BUFFER_FILL);
    if (ret == LSCP_OK && (events & LSCP_EVENT_CHANNEL_INFO))
        ret = _lscp_client_evt_request(pClient, 0, LSCP_EVENT_CHANNEL_INFO);
    if (ret == LSCP_OK && (events & LSCP_EVENT_MISCELLANEOUS))
        ret = _lscp_client_evt_request(pClient, 0, LSCP_EVENT_MISCELLANEOUS);

    // If necessary, close the alternate connection...
    if (pClient->events == LSCP_EVENT_NONE)
        lscp_socket_agent_free(&(pClient->evt));

    // Unlock this section down.
    lscp_mutex_unlock(pClient->mutex);

    return ret;
}


//-------------------------------------------------------------------------
// Client command protocol functions.

/**
 *  Loading an instrument:
 *  LOAD INSTRUMENT <filename> <instr-index> <sampler-channel>
 *
 *  @param pClient          Pointer to client instance structure.
 *  @param pszFileName      Instrument file name.
 *  @param iInstrIndex      Instrument index number.
 *  @param iSamplerChannel  Sampler Channel.
 *
 *  @returns LSCP_OK on success, LSCP_FAILED otherwise.
 */
lscp_status_t lscp_load_instrument ( lscp_client_t *pClient, const char *pszFileName, int iInstrIndex, int iSamplerChannel )
{
    char szQuery[LSCP_BUFSIZ];

    if (pszFileName == NULL || iSamplerChannel < 0)
        return LSCP_FAILED;

    sprintf(szQuery, "LOAD INSTRUMENT '%s' %d %d\r\n", pszFileName, iInstrIndex, iSamplerChannel);
    return lscp_client_query(pClient, szQuery);
}


/**
 *  Loading an instrument in the background (non modal):
 *  LOAD INSTRUMENT NON_MODAL <filename> <instr-index> <sampler-channel>
 *
 *  @param pClient          Pointer to client instance structure.
 *  @param pszFileName      Instrument file name.
 *  @param iInstrIndex      Instrument index number.
 *  @param iSamplerChannel  Sampler Channel.
 *
 *  @returns LSCP_OK on success, LSCP_FAILED otherwise.
 */
lscp_status_t lscp_load_instrument_non_modal ( lscp_client_t *pClient, const char *pszFileName, int iInstrIndex, int iSamplerChannel )
{
    char szQuery[LSCP_BUFSIZ];

    if (pszFileName == NULL || iSamplerChannel < 0)
        return LSCP_FAILED;

    sprintf(szQuery, "LOAD INSTRUMENT NON_MODAL '%s' %d %d\r\n", pszFileName, iInstrIndex, iSamplerChannel);
    return lscp_client_query(pClient, szQuery);
}


/**
 *  Loading a sampler engine:
 *  LOAD ENGINE <engine-name> <sampler-channel>
 *
 *  @param pClient          Pointer to client instance structure.
 *  @param pszEngineName    Engine name.
 *  @param iSamplerChannel  Sampler channel number.
 *
 *  @returns LSCP_OK on success, LSCP_FAILED otherwise.
 */
lscp_status_t lscp_load_engine ( lscp_client_t *pClient, const char *pszEngineName, int iSamplerChannel )
{
    char szQuery[LSCP_BUFSIZ];

    if (pszEngineName == NULL || iSamplerChannel < 0)
        return LSCP_FAILED;

    sprintf(szQuery, "LOAD ENGINE %s %d\r\n", pszEngineName, iSamplerChannel);
    return lscp_client_query(pClient, szQuery);
}


/**
 *  Current number of sampler channels:
 *  GET CHANNELS
 *
 *  @param pClient  Pointer to client instance structure.
 *
 *  @returns The current total number of sampler channels on success,
 *  -1 otherwise.
 */
int lscp_get_channels ( lscp_client_t *pClient )
{
    int iChannels = -1;

    // Lock this section up.
    lscp_mutex_lock(pClient->mutex);

    if (lscp_client_call(pClient, "GET CHANNELS\r\n") == LSCP_OK)
        iChannels = atoi(lscp_client_get_result(pClient));

    // Unlock this section doen.
    lscp_mutex_unlock(pClient->mutex);

    return iChannels;
}


/**
 *  List current sampler channels number identifiers:
 *  LIST CHANNELS
 *
 *  @param pClient  Pointer to client instance structure.
 *
 *  @returns An array of the sampler channels identifiers as positive integers,
 *  terminated with -1 on success, NULL otherwise.
 */
int *lscp_list_channels ( lscp_client_t *pClient )
{
    const char *pszSeps = ",";

    if (pClient == NULL)
        return NULL;
        
    // Lock this section up.
    lscp_mutex_lock(pClient->mutex);

    if (pClient->channels) {
        lscp_isplit_destroy(pClient->channels);
        pClient->channels = NULL;
    }

    if (lscp_client_call(pClient, "LIST CHANNELS\r\n") == LSCP_OK)
        pClient->channels = lscp_isplit_create(lscp_client_get_result(pClient), pszSeps);

    // Unlock this section down.
    lscp_mutex_unlock(pClient->mutex);

    return pClient->channels;
}


/**
 *  Adding a new sampler channel:
 *  ADD CHANNEL
 *
 *  @param pClient  Pointer to client instance structure.
 *
 *  @returns The new sampler channel number identifier,
 *  or -1 in case of failure.
 */
int lscp_add_channel ( lscp_client_t *pClient )
{
    int iSamplerChannel = -1;

    // Lock this section up.
    lscp_mutex_lock(pClient->mutex);

    if (lscp_client_call(pClient, "ADD CHANNEL\r\n") == LSCP_OK)
        iSamplerChannel = atoi(lscp_client_get_result(pClient));
        
    // Unlock this section down.
    lscp_mutex_unlock(pClient->mutex);

    return iSamplerChannel;
}


/**
 *  Removing a sampler channel:
 *  REMOVE CHANNEL <sampler-channel>
 *
 *  @param pClient          Pointer to client instance structure.
 *  @param iSamplerChannel  Sampler channel number.
 *
 *  @returns LSCP_OK on success, LSCP_FAILED otherwise.
 */
lscp_status_t lscp_remove_channel ( lscp_client_t *pClient, int iSamplerChannel )
{
    char szQuery[LSCP_BUFSIZ];

    if (iSamplerChannel < 0)
        return LSCP_FAILED;

    sprintf(szQuery, "REMOVE CHANNEL %d\r\n", iSamplerChannel);
    return lscp_client_query(pClient, szQuery);
}


/**
 *  Getting all available engines:
 *  GET AVAILABLE_ENGINES
 *
 *  @param pClient  Pointer to client instance structure.
 *
 *  @returns A NULL terminated array of engine name strings,
 *  or NULL in case of failure.
 */
const char **lscp_get_available_engines ( lscp_client_t *pClient )
{
    const char *pszSeps = ",";

    // Lock this section up.
    lscp_mutex_lock(pClient->mutex);

    if (pClient->engines) {
        lscp_szsplit_destroy(pClient->engines);
        pClient->engines = NULL;
    }

    if (lscp_client_call(pClient, "GET AVAILABLE_ENGINES\r\n") == LSCP_OK)
        pClient->engines = lscp_szsplit_create(lscp_client_get_result(pClient), pszSeps);

    // Unlock this section down.
    lscp_mutex_unlock(pClient->mutex);

    return (const char **) pClient->engines;
}


/**
 *  Getting information about an engine.
 *  GET ENGINE INFO <engine-name>
 *
 *  @param pClient          Pointer to client instance structure.
 *  @param pszEngineName    Engine name.
 *
 *  @returns A pointer to a @ref lscp_engine_info_t structure, with all the
 *  information of the given sampler engine, or NULL in case of failure.
 */
lscp_engine_info_t *lscp_get_engine_info ( lscp_client_t *pClient, const char *pszEngineName )
{
    lscp_engine_info_t *pEngineInfo;
    char szQuery[LSCP_BUFSIZ];
    const char *pszResult;
    const char *pszSeps = ":";
    const char *pszCrlf = "\r\n";
    char *pszToken;
    char *pch;

    if (pszEngineName == NULL)
        return NULL;

    // Lock this section up.
    lscp_mutex_lock(pClient->mutex);

    pEngineInfo = &(pClient->engine_info);
    lscp_engine_info_reset(pEngineInfo);

    sprintf(szQuery, "GET ENGINE INFO %s\r\n", pszEngineName);
    if (lscp_client_call(pClient, szQuery) == LSCP_OK) {
        pszResult = lscp_client_get_result(pClient);
        pszToken = lscp_strtok((char *) pszResult, pszSeps, &(pch));
        while (pszToken) {
            if (strcasecmp(pszToken, "DESCRIPTION") == 0) {
                pszToken = lscp_strtok(NULL, pszCrlf, &(pch));
                if (pszToken)
                    lscp_unquote_dup(&(pEngineInfo->description), &pszToken);
            }
            else if (strcasecmp(pszToken, "VERSION") == 0) {
                pszToken = lscp_strtok(NULL, pszCrlf, &(pch));
                if (pszToken)
                    lscp_unquote_dup(&(pEngineInfo->version), &pszToken);
            }
            pszToken = lscp_strtok(NULL, pszSeps, &(pch));
        }
    }
    else pEngineInfo = NULL;
    
    // Unlock this section down.
    lscp_mutex_unlock(pClient->mutex);

    return pEngineInfo;
}


/**
 *  Getting sampler channel informations:
 *  GET CHANNEL INFO <sampler-channel>
 *
 *  @param pClient          Pointer to client instance structure.
 *  @param iSamplerChannel  Sampler channel number.
 *
 *  @returns A pointer to a @ref lscp_channel_info_t structure, with all the
 *  information of the given sampler channel, or NULL in case of failure.
 */
lscp_channel_info_t *lscp_get_channel_info ( lscp_client_t *pClient, int iSamplerChannel )
{
    lscp_channel_info_t *pChannelInfo;
    char szQuery[LSCP_BUFSIZ];
    const char *pszResult;
    const char *pszSeps = ":";
    const char *pszCrlf = "\r\n";
    char *pszToken;
    char *pch;

    if (iSamplerChannel < 0)
        return NULL;

    // Lock this section up.
    lscp_mutex_lock(pClient->mutex);
    
    pChannelInfo = &(pClient->channel_info);
    lscp_channel_info_reset(pChannelInfo);

    sprintf(szQuery, "GET CHANNEL INFO %d\r\n", iSamplerChannel);
    if (lscp_client_call(pClient, szQuery) == LSCP_OK) {
        pszResult = lscp_client_get_result(pClient);
        pszToken = lscp_strtok((char *) pszResult, pszSeps, &(pch));
        while (pszToken) {
            if (strcasecmp(pszToken, "ENGINE_NAME") == 0) {
                pszToken = lscp_strtok(NULL, pszCrlf, &(pch));
                if (pszToken)
                    lscp_unquote_dup(&(pChannelInfo->engine_name), &pszToken);
            }
            else if (strcasecmp(pszToken, "AUDIO_OUTPUT_DEVICE") == 0) {
                pszToken = lscp_strtok(NULL, pszCrlf, &(pch));
                if (pszToken)
                    pChannelInfo->audio_device = atoi(lscp_ltrim(pszToken));
            }
            else if (strcasecmp(pszToken, "AUDIO_OUTPUT_CHANNELS") == 0) {
                pszToken = lscp_strtok(NULL, pszCrlf, &(pch));
                if (pszToken)
                    pChannelInfo->audio_channels = atoi(lscp_ltrim(pszToken));
            }
            else if (strcasecmp(pszToken, "AUDIO_OUTPUT_ROUTING") == 0) {
                pszToken = lscp_strtok(NULL, pszCrlf, &(pch));
                if (pszToken) {
                    if (pChannelInfo->audio_routing)
                        lscp_szsplit_destroy(pChannelInfo->audio_routing);
                    pChannelInfo->audio_routing = lscp_szsplit_create(pszToken, ",");
                }
            }
            else if (strcasecmp(pszToken, "INSTRUMENT_FILE") == 0) {
                pszToken = lscp_strtok(NULL, pszCrlf, &(pch));
                if (pszToken)
                    lscp_unquote_dup(&(pChannelInfo->instrument_file), &pszToken);
            }
            else if (strcasecmp(pszToken, "INSTRUMENT_NR") == 0) {
                pszToken = lscp_strtok(NULL, pszCrlf, &(pch));
                if (pszToken)
                    pChannelInfo->instrument_nr = atoi(lscp_ltrim(pszToken));
            }
            else if (strcasecmp(pszToken, "INSTRUMENT_STATUS") == 0) {
                pszToken = lscp_strtok(NULL, pszCrlf, &(pch));
                if (pszToken)
                    pChannelInfo->instrument_status = atoi(lscp_ltrim(pszToken));
            }
            else if (strcasecmp(pszToken, "MIDI_INPUT_DEVICE") == 0) {
                pszToken = lscp_strtok(NULL, pszCrlf, &(pch));
                if (pszToken)
                    pChannelInfo->midi_device = atoi(lscp_ltrim(pszToken));
            }
            else if (strcasecmp(pszToken, "MIDI_INPUT_PORT") == 0) {
                pszToken = lscp_strtok(NULL, pszCrlf, &(pch));
                if (pszToken)
                    pChannelInfo->midi_port = atoi(lscp_ltrim(pszToken));
            }
            else if (strcasecmp(pszToken, "MIDI_INPUT_CHANNEL") == 0) {
                pszToken = lscp_strtok(NULL, pszCrlf, &(pch));
                if (pszToken)
                    pChannelInfo->midi_channel = atoi(lscp_ltrim(pszToken));
            }
            else if (strcasecmp(pszToken, "VOLUME") == 0) {
                pszToken = lscp_strtok(NULL, pszCrlf, &(pch));
                if (pszToken)
                    pChannelInfo->volume = (float) atof(lscp_ltrim(pszToken));
            }
            pszToken = lscp_strtok(NULL, pszSeps, &(pch));
        }
    }
    else pChannelInfo = NULL;
    
    // Unlock this section up.
    lscp_mutex_unlock(pClient->mutex);

    return pChannelInfo;
}


/**
 *  Current number of active voices:
 *  GET CHANNEL VOICE_COUNT <sampler-channel>
 *
 *  @param pClient          Pointer to client instance structure.
 *  @param iSamplerChannel  Sampler channel number.
 *
 *  @returns The number of voices currently active, -1 in case of failure.
 */
int lscp_get_channel_voice_count ( lscp_client_t *pClient, int iSamplerChannel )
{
    char szQuery[LSCP_BUFSIZ];
    int iVoiceCount = -1;

    if (iSamplerChannel < 0)
        return iVoiceCount;

    // Lock this section up.
    lscp_mutex_lock(pClient->mutex);

    sprintf(szQuery, "GET CHANNEL VOICE_COUNT %d\r\n", iSamplerChannel);
    if (lscp_client_call(pClient, szQuery) == LSCP_OK)
        iVoiceCount = atoi(lscp_client_get_result(pClient));

    // Unlock this section down.
    lscp_mutex_unlock(pClient->mutex);

    return iVoiceCount;
}


/**
 *  Current number of active disk streams:
 *  GET CHANNEL STREAM_COUNT <sampler-channel>
 *
 *  @param pClient          Pointer to client instance structure.
 *  @param iSamplerChannel  Sampler channel number.
 *
 *  @returns The number of active disk streams on success, -1 otherwise.
 */
int lscp_get_channel_stream_count ( lscp_client_t *pClient, int iSamplerChannel )
{
    char szQuery[LSCP_BUFSIZ];
    int iStreamCount = -1;

    if (iSamplerChannel < 0)
        return iStreamCount;

    // Lock this section up.
    lscp_mutex_lock(pClient->mutex);

    sprintf(szQuery, "GET CHANNEL STREAM_COUNT %d\r\n", iSamplerChannel);
    if (lscp_client_call(pClient, szQuery) == LSCP_OK)
        iStreamCount = atoi(lscp_client_get_result(pClient));

    // Unlock this section down.
    lscp_mutex_unlock(pClient->mutex);

    return iStreamCount;
}


/**
 *  Current least usage of active disk streams.
 *
 *  @param pClient          Pointer to client instance structure.
 *  @param iSamplerChannel  Sampler channel number.
 *
 *  @returns The usage percentage of the least filled active disk stream
 *  on success, -1 otherwise.
 */
int lscp_get_channel_stream_usage ( lscp_client_t *pClient, int iSamplerChannel )
{
    char szQuery[LSCP_BUFSIZ];
    int  iStreamUsage = -1;
    const char *pszResult;
    const char *pszSeps = "[]%,";
    char *pszToken;
    char *pch;
    int   iStream;
    int   iPercent;

    if (iSamplerChannel < 0)
        return iStreamUsage;

    // Lock this section up.
    lscp_mutex_lock(pClient->mutex);

    iStream = 0;
    sprintf(szQuery, "GET CHANNEL BUFFER_FILL PERCENTAGE %d\r\n", iSamplerChannel);
    if (lscp_client_call(pClient, szQuery) == LSCP_OK) {
        pszResult = lscp_client_get_result(pClient);
        pszToken = lscp_strtok((char *) pszResult, pszSeps, &(pch));
        while (pszToken) {
            if (*pszToken) {
                // Skip stream id.
                pszToken = lscp_strtok(NULL, pszSeps, &(pch));
                if (pszToken == NULL)
                    break;
                // Get least buffer fill percentage.
                iPercent = atol(pszToken);
                if (iStreamUsage > iPercent || iStream == 0)
                    iStreamUsage = iPercent;
                iStream++;
            }
            pszToken = lscp_strtok(NULL, pszSeps, &(pch));
        }
    }

    // Unlock this section down.
    lscp_mutex_unlock(pClient->mutex);

    return iStreamUsage;
}


/**
 *  Current fill state of disk stream buffers:
 *  GET CHANNEL BUFFER_FILL {BYTES|PERCENTAGE} <sampler-channel>
 *
 *  @param pClient          Pointer to client instance structure.
 *  @param usage_type       Usage type to be returned, either
 *                          @ref LSCP_USAGE_BYTES, or
 *                          @ref LSCP_USAGE_PERCENTAGE.
 *  @param iSamplerChannel  Sampler channel number.
 *
 *  @returns A pointer to a @ref lscp_buffer_fill_t structure, with the
 *  information of the current disk stream buffer fill usage, for the given
 *  sampler channel, or NULL in case of failure.
 */
lscp_buffer_fill_t *lscp_get_channel_buffer_fill ( lscp_client_t *pClient, lscp_usage_t usage_type, int iSamplerChannel )
{
    lscp_buffer_fill_t *pBufferFill;
    char szQuery[LSCP_BUFSIZ];
    int iStreamCount;
    const char *pszUsageType = (usage_type == LSCP_USAGE_BYTES ? "BYTES" : "PERCENTAGE");
    const char *pszResult;
    const char *pszSeps = "[]%,";
    char *pszToken;
    char *pch;
    int   iStream;

    // Retrieve a channel stream estimation.
    iStreamCount = lscp_get_channel_stream_count(pClient, iSamplerChannel);
    if (pClient->iStreamCount < 0)
        return NULL;

    // Lock this section up.
    lscp_mutex_lock(pClient->mutex);

    // Check if we need to reallocate the stream usage array.
    if (pClient->iStreamCount != iStreamCount) {
        if (pClient->buffer_fill)
            free(pClient->buffer_fill);
        if (iStreamCount > 0)
            pClient->buffer_fill = (lscp_buffer_fill_t *) malloc(iStreamCount * sizeof(lscp_buffer_fill_t));
        else
            pClient->buffer_fill = NULL;
        pClient->iStreamCount = iStreamCount;
    }

    // Get buffer fill usage...
    pBufferFill = pClient->buffer_fill;
    if (pBufferFill && iStreamCount > 0) {
        iStream = 0;
        pBufferFill = pClient->buffer_fill;
        sprintf(szQuery, "GET CHANNEL BUFFER_FILL %s %d\r\n", pszUsageType, iSamplerChannel);
        if (lscp_client_call(pClient, szQuery) == LSCP_OK) {
            pszResult = lscp_client_get_result(pClient);
            pszToken = lscp_strtok((char *) pszResult, pszSeps, &(pch));
            while (pszToken && iStream < pClient->iStreamCount) {
                if (*pszToken) {
                    pBufferFill[iStream].stream_id = atol(pszToken);
                    pszToken = lscp_strtok(NULL, pszSeps, &(pch));
                    if (pszToken == NULL)
                        break;
                    pBufferFill[iStream].stream_usage = atol(pszToken);
                    iStream++;
                }
                pszToken = lscp_strtok(NULL, pszSeps, &(pch));
            }
        }   // Reset the usage, whatever it was before.
        else while (iStream < pClient->iStreamCount)
            pBufferFill[iStream++].stream_usage = 0;
    }
    
    // Unlock this section down.
    lscp_mutex_unlock(pClient->mutex);

    return pBufferFill;
}


/**
 *  Setting audio output type:
 *  SET CHANNEL AUDIO_OUTPUT_TYPE <sampler-channel> <audio-output-type>
 *
 *  @param pClient          Pointer to client instance structure.
 *  @param iSamplerChannel  Sampler channel number.
 *  @param pszAudioDriver   Audio output driver type (e.g. "ALSA" or "JACK").
 */
lscp_status_t lscp_set_channel_audio_type ( lscp_client_t *pClient, int iSamplerChannel, const char *pszAudioDriver )
{
    char szQuery[LSCP_BUFSIZ];

    if (iSamplerChannel < 0 || pszAudioDriver == NULL)
        return LSCP_FAILED;

    sprintf(szQuery, "SET CHANNEL AUDIO_OUTPUT_TYPE %d %s\r\n", iSamplerChannel, pszAudioDriver);
    return lscp_client_query(pClient, szQuery);
}


/**
 *  Setting audio output device:
 *  SET CHANNEL AUDIO_OUTPUT_DEVICE <sampler-channel> <device-id>
 *
 *  @param pClient          Pointer to client instance structure.
 *  @param iSamplerChannel  Sampler channel number.
 *  @param iAudioDevice     Audio output device number identifier.
 */
lscp_status_t lscp_set_channel_audio_device ( lscp_client_t *pClient, int iSamplerChannel, int iAudioDevice )
{
    char szQuery[LSCP_BUFSIZ];

    if (iSamplerChannel < 0 || iAudioDevice < 0)
        return LSCP_FAILED;

    sprintf(szQuery, "SET CHANNEL AUDIO_OUTPUT_DEVICE %d %d\r\n", iSamplerChannel, iAudioDevice);
    return lscp_client_query(pClient, szQuery);
}


/**
 *  Setting audio output channel:
 *  SET CHANNEL AUDIO_OUTPUT_CHANNEL <sampler-channel> <audio-output-chan> <audio-input-chan>
 *
 *  @param pClient          Pointer to client instance structure.
 *  @param iSamplerChannel  Sampler channel number.
 *  @param iAudioOut        Audio output device channel to be routed from.
 *  @param iAudioIn         Audio output device channel to be routed into.
 *
 *  @returns LSCP_OK on success, LSCP_FAILED otherwise.
 */
lscp_status_t lscp_set_channel_audio_channel ( lscp_client_t *pClient, int iSamplerChannel, int iAudioOut, int iAudioIn )
{
    char szQuery[LSCP_BUFSIZ];

    if (iSamplerChannel < 0 || iAudioOut < 0 || iAudioIn < 0)
        return LSCP_FAILED;

    sprintf(szQuery, "SET CHANNEL AUDIO_OUTPUT_CHANNELS %d %d %d\r\n", iSamplerChannel, iAudioOut, iAudioIn);
    return lscp_client_query(pClient, szQuery);
}


/**
 *  Setting MIDI input type:
 *  SET CHANNEL MIDI_INPUT_TYPE <sampler-channel> <midi-input-type>
 *
 *  @param pClient          Pointer to client instance structure.
 *  @param iSamplerChannel  Sampler channel number.
 *  @param pszMidiDriver    MIDI input driver type (e.g. "ALSA").
 *
 *  @returns LSCP_OK on success, LSCP_FAILED otherwise.
 */
lscp_status_t lscp_set_channel_midi_type ( lscp_client_t *pClient, int iSamplerChannel, const char *pszMidiDriver )
{
    char szQuery[LSCP_BUFSIZ];

    if (iSamplerChannel < 0 || pszMidiDriver == NULL)
        return LSCP_FAILED;

    sprintf(szQuery, "SET CHANNEL MIDI_INPUT_TYPE %d %s\r\n", iSamplerChannel, pszMidiDriver);
    return lscp_client_query(pClient, szQuery);
}


/**
 *  Setting MIDI input device:
 *  SET CHANNEL MIDI_INPUT_DEVICE <sampler-channel> <device-id>
 *
 *  @param pClient          Pointer to client instance structure.
 *  @param iSamplerChannel  Sampler channel number.
 *  @param iMidiDevice      MIDI input device number identifier.
 */
lscp_status_t lscp_set_channel_midi_device ( lscp_client_t *pClient, int iSamplerChannel, int iMidiDevice )
{
    char szQuery[LSCP_BUFSIZ];

    if (iSamplerChannel < 0 || iMidiDevice < 0)
        return LSCP_FAILED;

    sprintf(szQuery, "SET CHANNEL MIDI_INPUT_DEVICE %d %d\r\n", iSamplerChannel, iMidiDevice);
    return lscp_client_query(pClient, szQuery);
}


/**
 *  Setting MIDI input port:
 *  SET CHANNEL MIDI_INPUT_PORT <sampler-channel> <midi-input-port>
 *
 *  @param pClient          Pointer to client instance structure.
 *  @param iSamplerChannel  Sampler channel number.
 *  @param iMidiPort        MIDI input driver virtual port number.
 *
 *  @returns LSCP_OK on success, LSCP_FAILED otherwise.
 */
lscp_status_t lscp_set_channel_midi_port ( lscp_client_t *pClient, int iSamplerChannel, int iMidiPort )
{
    char szQuery[LSCP_BUFSIZ];

    if (iSamplerChannel < 0 || iMidiPort < 0)
        return LSCP_FAILED;

    sprintf(szQuery, "SET CHANNEL MIDI_INPUT_PORT %d %d\r\n", iSamplerChannel, iMidiPort);
    return lscp_client_query(pClient, szQuery);
}


/**
 *  Setting MIDI input channel:
 *  SET CHANNEL MIDI_INPUT_CHANNEL <sampler-channel> <midi-input-chan>
 *
 *  @param pClient          Pointer to client instance structure.
 *  @param iSamplerChannel  Sampler channel number.
 *  @param iMidiChannel     MIDI channel number to listen (1-16) or
 *                          zero (0) to listen on all channels.
 *
 *  @returns LSCP_OK on success, LSCP_FAILED otherwise.
 */
lscp_status_t lscp_set_channel_midi_channel ( lscp_client_t *pClient, int iSamplerChannel, int iMidiChannel )
{
    char szQuery[LSCP_BUFSIZ];

    if (iSamplerChannel < 0 || iMidiChannel < 0 || iMidiChannel > 16)
        return LSCP_FAILED;

    if (iMidiChannel > 0)
        sprintf(szQuery, "SET CHANNEL MIDI_INPUT_CHANNEL %d %d\r\n", iSamplerChannel, iMidiChannel);
    else
        sprintf(szQuery, "SET CHANNEL MIDI_INPUT_CHANNEL %d ALL\r\n", iSamplerChannel);
    return lscp_client_query(pClient, szQuery);
}


/**
 *  Setting channel volume:
 *  SET CHANNEL VOLUME <sampler-channel> <volume>
 *
 *  @param pClient          Pointer to client instance structure.
 *  @param iSamplerChannel  Sampler channel number.
 *  @param fVolume          Sampler channel volume as a positive floating point
 *                          number, where a value less than 1.0 for attenuation,
 *                          and greater than 1.0 for amplification.
 *
 *  @returns LSCP_OK on success, LSCP_FAILED otherwise.
 */
lscp_status_t lscp_set_channel_volume ( lscp_client_t *pClient, int iSamplerChannel, float fVolume )
{
    char szQuery[LSCP_BUFSIZ];

    if (iSamplerChannel < 0 || fVolume < 0.0)
        return LSCP_FAILED;

    sprintf(szQuery, "SET CHANNEL VOLUME %d %g\r\n", iSamplerChannel, fVolume);
    return lscp_client_query(pClient, szQuery);
}


/**
 *  Resetting a sampler channel:
 *  RESET CHANNEL <sampler-channel>
 *
 *  @param pClient          Pointer to client instance structure.
 *  @param iSamplerChannel  Sampler channel number.
 *
 *  @returns LSCP_OK on success, LSCP_FAILED otherwise.
 */
lscp_status_t lscp_reset_channel ( lscp_client_t *pClient, int iSamplerChannel )
{
    char szQuery[LSCP_BUFSIZ];

    if (iSamplerChannel < 0)
        return LSCP_FAILED;

    sprintf(szQuery, "RESET CHANNEL %d\r\n", iSamplerChannel);
    return lscp_client_query(pClient, szQuery);
}


// end of client.c
