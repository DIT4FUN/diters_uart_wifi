/*
 * client.c
 *
 *  Created on: 2017年9月15日
 *      Author: Aunknownthat
 */
/**
 ******************************************************************************
 * @file    main.c
 * @author
 * @version V1.0.0
 * @date
 * @brief
 ******************************************************************************
 *
 *  The MIT License
 *  Copyright (c) 2016 MXCHIP Inc.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is furnished
 *  to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 *  WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR
 *  IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 ******************************************************************************
 */

#include "mico.h"
#include "SppProtocol.h"
#include "client.h"
#include "SocketUtils.h"



static char *ap_ssid = "mxchipWifiUart";
static char *ap_key = "ditersMxchip";

#define wifi_softap_log(M, ...) custom_log("WIFI", M, ##__VA_ARGS__)
#define wifi_station_log(M, ...) custom_log("WIFI", M, ##__VA_ARGS__)

static char tcp_remote_ip[16] = "192.168.0.1"; /*remote ip address*/
#define SERVER_PORT 20000 /*set up a tcp server,port at 20000*/
/* Define thread stack size */
#define STACK_SIZE_UART_RECV_THREAD           0x2A0

#define wlanBufferLen                       1024
#define UART_RECV_TIMEOUT                   500
#define UART_ONE_PACKAGE_LENGTH             1024
#define UART_BUFFER_LENGTH                  2048

volatile ring_buffer_t rx_buffer;
volatile uint8_t rx_data[UART_BUFFER_LENGTH];

#define CLOUD_RETRY  1
static bool _wifiConnected = false;
static mico_semaphore_t  _wifiConnected_sem = NULL;

void clientNotify_WifiStatusHandler(int event, void* arg )
{
  (void)arg;
  switch (event) {
  case NOTIFY_STATION_UP:
    _wifiConnected = true;
    wifi_station_log("station up!");
    mico_rtos_set_semaphore(&_wifiConnected_sem);
    break;
  case NOTIFY_STATION_DOWN:
    _wifiConnected = false;
    wifi_station_log("station down!");
    break;
  default:
    break;
  }
  return;
}

void remoteTcpClient_thread(uint32_t inContext)
{
  OSStatus err = kUnknownErr;
  int len;
  app_context_t *context = (app_context_t *)inContext;
  struct sockaddr_in addr;
  fd_set readfds;
  fd_set writeSet;
  char ipstr[16];
  struct timeval t;
  int remoteTcpClient_fd = -1;
  uint8_t *inDataBuffer = NULL;
  int eventFd = -1;
  mico_queue_t queue;
  socket_msg_t *msg;
  LinkStatusTypeDef wifi_link;
  int sent_len, errno;
  struct hostent* hostent_content = NULL;
  char **pptr = NULL;
  struct in_addr in_addr;

  mico_rtos_init_semaphore(&_wifiConnected_sem, 1);



  inDataBuffer = malloc(wlanBufferLen);
  require_action(inDataBuffer, exit, err = kNoMemoryErr);

  err = micoWlanGetLinkStatus( &wifi_link );
  require_noerr( err, exit );

  if( wifi_link.is_connected == true )
  {
    _wifiConnected = true;
  }
  while(1) {
    if(remoteTcpClient_fd == -1 ) {
      if(_wifiConnected == false){
        require_action_quiet(mico_rtos_get_semaphore(&_wifiConnected_sem, 200000) == kNoErr, Continue, err = kTimeoutErr);
      }


      remoteTcpClient_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = inet_addr(tcp_remote_ip);
      addr.sin_port = htons(SERVER_PORT);

      err = connect(remoteTcpClient_fd, (struct sockaddr *)&addr, sizeof(addr));
      require_noerr_quiet(err, ReConnWithDelay);
      wifi_station_log("Remote server connected at port: %d, fd: %d",  context->appConfig->remoteServerPort,
                 remoteTcpClient_fd);

      err = socket_queue_create(context, &queue);
      require_noerr( err, exit );
      eventFd = mico_create_event_fd(queue);
      if (eventFd < 0) {
          wifi_station_log("create event fd error");
        socket_queue_delete(context, &queue);
        goto ReConnWithDelay;
      }
    }else{
      FD_ZERO(&readfds);
      FD_SET(remoteTcpClient_fd, &readfds);
      FD_SET(eventFd, &readfds);
      t.tv_sec = 4;
      t.tv_usec = 0;
      select( Max(remoteTcpClient_fd, eventFd) + 1, &readfds, NULL, NULL, &t);
      /* send UART data */
      if (FD_ISSET( eventFd, &readfds )) {// have data
        FD_ZERO(&writeSet );
        FD_SET(remoteTcpClient_fd, &writeSet );
        t.tv_usec = 100*1000; // max wait 100ms.
        select(1 + remoteTcpClient_fd, NULL, &writeSet, NULL, &t);
        if ((FD_ISSET(remoteTcpClient_fd, &writeSet )) &&
            (kNoErr == mico_rtos_pop_from_queue( &queue, &msg, 0))) {
           sent_len = write(remoteTcpClient_fd, msg->data, msg->len);
           if (sent_len <= 0) {
            len = sizeof(errno);
            getsockopt(remoteTcpClient_fd, SOL_SOCKET, SO_ERROR, &errno, (socklen_t *)&len);

            socket_msg_free(msg);
            if (errno != ENOMEM) {
                wifi_station_log("write error, fd: %d, errno %d", remoteTcpClient_fd,errno );
                goto ReConnWithDelay;
            }
           } else {
                    socket_msg_free(msg);
                }
            }
      }
      /*recv wlan data using remote client fd*/
      if (FD_ISSET(remoteTcpClient_fd, &readfds)) {
        len = recv(remoteTcpClient_fd, inDataBuffer, wlanBufferLen, 0);
        if(len <= 0) {
            wifi_station_log("Remote client closed, fd: %d", remoteTcpClient_fd);
          goto ReConnWithDelay;
        }
        sppWlanCommandProcess(inDataBuffer, &len, remoteTcpClient_fd, context);
      }

    Continue:
      continue;

    ReConnWithDelay:
        if (eventFd >= 0) {
          mico_delete_event_fd(eventFd);
          eventFd = -1;
          socket_queue_delete(context, &queue);
        }
        if(remoteTcpClient_fd != -1){
          SocketClose(&remoteTcpClient_fd);
        }
        mico_rtos_thread_sleep(CLOUD_RETRY);
    }
  }

exit:
  if(inDataBuffer) free(inDataBuffer);
  wifi_station_log("Exit: Remote TCP client exit with err = %d", err);
  mico_rtos_delete_thread(NULL);
  return;
}


/*
* copy to buf, return len = datalen+10
*/
size_t _uart_get_one_packet(uint8_t* inBuf, int inBufLen)
{

  int datalen;

  while(1) {
    if( MicoUartRecv( UART_FOR_APP, inBuf, inBufLen, UART_RECV_TIMEOUT) == kNoErr){
      return inBufLen;
    }
   else{
     datalen = MicoUartGetLengthInBuffer( UART_FOR_APP );
     if(datalen){
       MicoUartRecv(UART_FOR_APP, inBuf, datalen, UART_RECV_TIMEOUT);
       return datalen;
     }
   }
  }

}

void uartRecv_thread( mico_thread_arg_t inContext)
{
    app_context_t *Context = (app_context_t *)inContext;
     int recvlen;
     uint8_t *inDataBuffer;

     inDataBuffer = malloc(UART_ONE_PACKAGE_LENGTH);
     require(inDataBuffer, exit);

     while(1) {
       recvlen = _uart_get_one_packet(inDataBuffer, UART_ONE_PACKAGE_LENGTH);
       if (recvlen <= 0)
         continue;
       sppUartCommandProcess(inDataBuffer, recvlen, Context);
     }

   exit:
     if(inDataBuffer) free(inDataBuffer);
     mico_rtos_delete_thread(NULL);
}


int application_start( void )
{

    OSStatus err = kNoErr;
    mico_uart_config_t uart_config;
    network_InitTypeDef_adv_st  wNetConfigAdv;

       app_context_t* app_context;
       mico_Context_t* mico_context;

       /* Create application context */
       app_context = (app_context_t *) calloc( 1, sizeof(app_context_t) );
       require_action( app_context, exit, err = kNoMemoryErr );


       /* Regisist notifications */
       err = mico_system_notify_register( mico_notify_WIFI_STATUS_CHANGED, (void *)clientNotify_WifiStatusHandler, NULL );
       require_noerr( err, exit );

       /* Create mico system context and read application's config data from flash */
       mico_context = mico_system_context_init( sizeof(application_config_t) );
       app_context->appConfig = mico_system_context_get_user_data( mico_context );


       /* mico system initialize */
       err = mico_system_init( mico_context );
       require_noerr( err, exit );

       /* Protocol initialize */
       sppProtocolInit( app_context );

       /*UART receive thread*/
       uart_config.baud_rate = 115200;
       uart_config.data_width = DATA_WIDTH_8BIT;
       uart_config.parity = NO_PARITY;
       uart_config.stop_bits = STOP_BITS_1;
       uart_config.flow_control = FLOW_CONTROL_DISABLED;
       uart_config.flags = UART_WAKEUP_DISABLE;

      ring_buffer_init( (ring_buffer_t *) &rx_buffer, (uint8_t *) rx_data, UART_BUFFER_LENGTH );
      MicoUartInitialize( UART_FOR_APP, &uart_config, (ring_buffer_t *) &rx_buffer );


       /* Initialize wlan parameters */
       memset( &wNetConfigAdv, 0x0, sizeof(wNetConfigAdv) );
       strcpy((char*)wNetConfigAdv.ap_info.ssid, ap_ssid);   /* wlan ssid string */
       strcpy((char*)wNetConfigAdv.key, ap_key);                /* wlan key string or hex data in WEP mode */
       wNetConfigAdv.key_len = strlen(ap_key);                  /* wlan key length */
       wNetConfigAdv.ap_info.security = SECURITY_TYPE_AUTO;          /* wlan security mode */
       wNetConfigAdv.ap_info.channel = 0;                            /* Select channel automatically */
       wNetConfigAdv.dhcpMode = DHCP_Client;                         /* Fetch Ip address from DHCP server */
       wNetConfigAdv.wifi_retry_interval = 100;                      /* Retry interval after a failure connection */


       /* Connect Now! */
       wifi_station_log("connecting to %s...", wNetConfigAdv.ap_info.ssid);
       micoWlanStartAdv(&wNetConfigAdv);



       err = mico_rtos_create_thread( NULL, MICO_APPLICATION_PRIORITY, "UART Recv", uartRecv_thread,
                                                  STACK_SIZE_UART_RECV_THREAD,  (mico_thread_arg_t)app_context );
       require_noerr_string( err, exit, "ERROR: Unable to start the uart recv thread.");


       /* Start TCP server listener thread*/
       err = mico_rtos_create_thread( NULL, MICO_APPLICATION_PRIORITY, "TCP_client", remoteTcpClient_thread,0x500,  (mico_thread_arg_t)app_context  );
       require_noerr_string( err, exit, "ERROR: Unable to start the tcp client thread." );


       exit:
       mico_rtos_delete_thread( NULL );
       return err;
}










