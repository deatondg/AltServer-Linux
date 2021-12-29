#include "PhoneHelper.h"
#include "phone/global.h"
#include <unistd.h>
#include <pthread.h>
#include <string.h>

#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/debugserver.h>
#include <libimobiledevice/heartbeat.h>
#include <libimobiledevice/installation_proxy.h>
#include <libimobiledevice/lockdown.h>
#include <libimobiledevice/mobile_image_mounter.h>
#include <libimobiledevice/sbservices.h>
#include <libimobiledevice/service.h>
#include "common/userpref.h"
#include "common/utils.h"

#ifndef TOOL_NAME
#define TOOL_NAME "AltServerLinux"
#endif
#include "common.h"

#include <miniupnpc/miniwget.h>
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/portlistingparse.h>
#include <miniupnpc/upnperrors.h>
struct UPNPUrls _upnpUrls = { 0 };
struct IGDdatas _upnpData = { 0 };
char upnpExternalAddr[40] = { 0 };

int initUPnP() {
	int error = 0;
    char lanaddr[64] = "unset";
    int upnpRet = 0;
    if (1) {
        struct UPNPDev *devlist = upnpDiscover(2000, NULL, "", 0, 0, 2, &error);
        upnpRet = UPNP_GetValidIGD(devlist, &_upnpUrls, &_upnpData, lanaddr, sizeof(lanaddr));
    } else {
        //upnpRet = 1; UPNP_GetIGDFromUrl("http://192.168.1.1:1900/igd.xml", &_upnpUrls, &_upnpData, lanaddr, sizeof(lanaddr));
    }
    upnpUrls = &_upnpUrls;
    upnpData = &_upnpData;
    if (upnpRet == 1) {
        DEBUG_PRINT("Got good upnp igd: %s", _upnpUrls.controlURL);
    } else if (upnpRet == 2) {
        DEBUG_PRINT("Got not-connected igd: %s", _upnpUrls.controlURL);
    } else {
        DEBUG_PRINT("Found unknown upnp dev: %s", _upnpUrls.controlURL);
    }
    if (upnpRet != 1) {
        return 0;
    }

    char externalIPAddress[40] = { 0 };
    int r = UPNP_GetExternalIPAddress(upnpUrls->controlURL,
	                          upnpData->first.servicetype,
							  externalIPAddress);
    if(r != UPNPCOMMAND_SUCCESS) {
		DEBUG_PRINT("GetExternalIPAddress failed. (errorcode=%d)\n", r);
        return 0;
    }
    DEBUG_PRINT("Got ExternalIPAddress = %s\n", externalIPAddress);
    strcpy(upnpExternalAddr, externalIPAddress);
    return 1;
}

int initGlobalDevice() {
    idevice_error_t derr = IDEVICE_E_SUCCESS;
    if ((derr = idevice_new_with_options(&g_device, pairUDID, IDEVICE_LOOKUP_NETWORK)) != IDEVICE_E_SUCCESS) {
        DEBUG_PRINT("Failed to create device: %d", derr);
        return 0;
    }
    return 1;
}


int do_heartbeat(void *_client) {
    heartbeat_client_t client = _client;
    plist_t ping;
    uint64_t interval = 15;
    //DEBUG_PRINT("Timer run!");
    if (heartbeat_receive_with_timeout(client, &ping, (uint32_t)interval * 1000) != HEARTBEAT_E_SUCCESS) {
        DEBUG_PRINT("Did not recieve ping, canceling timer!");
        return 0;
    }
    plist_get_uint_val(plist_dict_get_item(ping, "Interval"), &interval);
    //DEBUG_PRINT("Set new timer interval: %lu!", interval);
    //DEBUG_PRINT("Sending heartbeat.");
    heartbeat_send(client, ping);
    plist_free(ping);
    return (interval);
}


int initHeartbeat(void **_hbclient) {
    //idevice_set_debug_level(1);
    
    lockdownd_error_t lerr = LOCKDOWN_E_SUCCESS;
    DEBUG_PRINT("Start hb...");
    heartbeat_client_t hbclient;
    heartbeat_error_t err = HEARTBEAT_E_UNKNOWN_ERROR;
    int herr = heartbeat_client_start_service(g_device, &hbclient, TOOL_NAME);
    if (herr != HEARTBEAT_E_SUCCESS) {
        DEBUG_PRINT("Failed to create heartbeat service: %d", herr);
        return 0;
    }
    *_hbclient = hbclient;
    //idevice_set_debug_level(0);
    return 1;
}


static void heartbeat_thread(heartbeat_client_t client) {
    while (1) {
        int interval = do_heartbeat(client);
        if (!interval) break;
        sleep(interval);
    }
}
int startHeartbeat(void *hbclient) {
    pthread_t thHeartbeat;
    //pthread_create(&thHeartbeat, NULL, (void * (*)(void *))heartbeat_thread, hbclient);
    int ret = fork();
    if (ret == 0) {
        heartbeat_thread(hbclient);
    }
}

void setupPairInfo(const char *udid, const char *ipaddr, const char *pairDataFile) {
    DEBUG_PRINT("Setup pairInfo...");
    strcpy(pairUDID, udid);
    strcpy(pairDeviceAddress, ipaddr);
    FILE *f = fopen(pairDataFile, "rb");
    fseek(f, 0, SEEK_END);
    pairDataLen = ftell(f);
    fseek(f, 0, SEEK_SET);
    fread(pairData, 1, pairDataLen, f);
}

// int main(int argc, char *argv[]) {
//     setvbuf(stdin, NULL, _IONBF, 0); 
//     setvbuf(stdout, NULL, _IONBF, 0); 
//     setvbuf(stderr, NULL, _IONBF, 0); 

//     srand(time(NULL));
//     DEBUG_PRINT("Setup pairInfo...");
//     strcpy(pairUDID, argv[1]);
//     strcpy(pairDeviceAddress, argv[2]);
//     FILE *f = fopen(argv[3], "rb");
//     fseek(f, 0, SEEK_END);
//     pairDataLen = ftell(f);
//     fseek(f, 0, SEEK_SET);
//     fread(pairData, 1, pairDataLen, f);
    
//     if (!initUPnP()) {
//         DEBUG_PRINT("failed to init upnp! exitting...");
//         return 1;
//     }
//     DEBUG_PRINT("upnp init successfully!");
    
//     DEBUG_PRINT("Connect device...");
//     if (!initHeartbeat()) {
//         DEBUG_PRINT("failed to init heartbeat! exitting...");
//         return 1;
//     }

//     // DEBUG_PRINT("Start tool...");
//     // tool_main(argc - 3, argv + 3);
// }
