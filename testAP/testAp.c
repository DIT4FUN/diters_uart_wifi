#include "mico.h"

#define wifi_softap_log(M, ...) custom_log("WIFI", M, ##__VA_ARGS__)

static char *ap_ssid = "mxchip_zfw";
static char *ap_key = "12345678";

int application_start( void )
{
  OSStatus err = kNoErr;
  network_InitTypeDef_st wNetConfig;
 
  err = mico_system_init( mico_system_context_init( 0 ) );
  require_noerr( err, exit ); 
  
  memset(&wNetConfig, 0x0, sizeof(network_InitTypeDef_st));
  
  strcpy((char*)wNetConfig.wifi_ssid, ap_ssid);
  strcpy((char*)wNetConfig.wifi_key, ap_key);
  
  wNetConfig.wifi_mode = Soft_AP;
  wNetConfig.dhcpMode = DHCP_Server;
  wNetConfig.wifi_retry_interval = 100;
  strcpy((char*)wNetConfig.local_ip_addr, "192.168.0.1");
  strcpy((char*)wNetConfig.net_mask, "255.255.255.0");
  strcpy((char*)wNetConfig.dnsServer_ip_addr, "192.168.0.1");
  
  wifi_softap_log("ssid:%s  key:%s", wNetConfig.wifi_ssid, wNetConfig.wifi_key);\
  micoWlanStart(&wNetConfig);

exit:  
  mico_rtos_delete_thread(NULL);
  return err;
}


