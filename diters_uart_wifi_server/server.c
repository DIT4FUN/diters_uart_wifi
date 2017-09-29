/**
 ******************************************************************************
 * @file    server.c
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
#include "server.h"


static int  client_fd  = -1;

static char *ap_ssid = "mxchipWifiUart";
static char *ap_key = "ditersMxchip";


#define wifi_softap_log(M, ...) custom_log("WIFI", M, ##__VA_ARGS__)
#define SERVER_PORT 20000 /*set up a tcp server,port at 20000*/

/* Define thread stack size */
#define STACK_SIZE_UART_RECV_THREAD         0x2A0
#define wlanBufferLen                       1024
#define UART_RECV_TIMEOUT                   500
#define UART_ONE_PACKAGE_LENGTH             1024
#define UART_BUFFER_LENGTH                  2048

volatile ring_buffer_t rx_buffer;
volatile uint8_t rx_data[UART_BUFFER_LENGTH];


static mico_Context_t *Context;
static app_context_t *context;

void tcp_client_thread( mico_thread_arg_t inFd )
{
    OSStatus err;
     int clientFd = *(int *)inFd;
     uint8_t *inDataBuffer = NULL;
     int len;
     fd_set readfds;
     fd_set writeSet;
     struct timeval t;
     int eventFd = -1;
     mico_queue_t queue;
     socket_msg_t *msg;
     int sent_len, errno;

     inDataBuffer = malloc(wlanBufferLen);
     require_action(inDataBuffer, exit, err = kNoMemoryErr);

     err = socket_queue_create(context, &queue);
     require_noerr( err, exit );
     eventFd = mico_create_event_fd(queue);
     if (eventFd < 0) {
         wifi_softap_log("create event fd error");
       goto exit_with_queue;
     }

     t.tv_sec = 4;
     t.tv_usec = 0;

     while(1){

       FD_ZERO(&readfds);
       FD_SET(clientFd, &readfds);
       FD_SET(eventFd, &readfds);

       select( Max(clientFd, eventFd) + 1, &readfds, NULL, NULL, &t);
       /* send UART data */
       if (FD_ISSET( eventFd, &readfds )) { // have data and can write
           FD_ZERO(&writeSet );
           FD_SET(clientFd, &writeSet );
           t.tv_usec = 100*1000; // max wait 100ms.
           select(clientFd + 1, NULL, &writeSet, NULL, &t);
           if((FD_ISSET( clientFd, &writeSet )) &&
               (kNoErr == mico_rtos_pop_from_queue( &queue, &msg, 0))) {
              sent_len = write(clientFd, msg->data, msg->len);
              if (sent_len <= 0) {
                 len = sizeof(errno);
                 getsockopt(clientFd, SOL_SOCKET, SO_ERROR, &errno, (socklen_t *)&len);
                 socket_msg_free(msg);
                 wifi_softap_log("write error, fd: %d, errno %d", clientFd, errno );
                 if (errno != ENOMEM) {
                     goto exit_with_queue;
                 }
              } else {
                     socket_msg_free(msg);
                 }
              }
           }

       /*Read data from tcp clients and process these data using HA protocol */
       if (FD_ISSET(clientFd, &readfds)) {
         len = recv(clientFd, inDataBuffer, wlanBufferLen, 0);
         require_action_quiet(len>0, exit_with_queue, err = kConnectionErr);
         sppWlanCommandProcess(inDataBuffer, &len, clientFd, context);
       }
     }

   exit_with_queue:
       len = sizeof(errno);
       getsockopt(clientFd, SOL_SOCKET, SO_ERROR, &errno, (socklen_t *)&len);
       wifi_softap_log("Exit: Client exit with err = %d, socket errno %d", err, errno);
       if (eventFd >= 0) {
           mico_delete_event_fd(eventFd);
       }
       socket_queue_delete(context, &queue);
   exit:
       SocketClose(&clientFd);
       if(inDataBuffer) free(inDataBuffer);
       mico_rtos_delete_thread(NULL);
       return;
}

/* TCP server listener thread */
void tcp_server_thread( mico_thread_arg_t inContext )
{

    OSStatus err = kNoErr;
    int client_fd;
    context = (app_context_t *)inContext;
    struct sockaddr_in addr;
    int sockaddr_t_size;

    char client_ip_str[16];
    int tcp_listen_fd  = -1;
    fd_set readfds;

    tcp_listen_fd = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP );
    require_action( IsValidSocket( tcp_listen_fd ), exit, err = kNoResourcesErr );

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;/* Accept conenction request on all network interface */
    addr.sin_port = htons( SERVER_PORT );/* Server listen on port: 20000 */

    err = bind( tcp_listen_fd, (struct sockaddr *) &addr, sizeof(addr) );
    require_noerr( err, exit );

    err = listen( tcp_listen_fd, 0 );
    require_noerr( err, exit );

    wifi_softap_log("tcp server listening");
    while ( 1 )
    {
        FD_ZERO( &readfds );
        FD_SET( tcp_listen_fd, &readfds );

        require( select( tcp_listen_fd + 1, &readfds, NULL, NULL, NULL) >= 0, exit );

        if ( FD_ISSET( tcp_listen_fd, &readfds ) )
        {
            client_fd  = accept( tcp_listen_fd, (struct sockaddr *) &addr, (socklen_t *)&sockaddr_t_size );
            if ( IsValidSocket( client_fd  ) )
            {
//                inet_ntoa( client_ip_str, client_addr.s_ip );
                strcpy( client_ip_str, inet_ntoa( addr.sin_addr ) );
                wifi_softap_log( "TCP Client %s:%d connected, fd: %d", client_ip_str, addr.sin_port, client_fd  );
                if ( kNoErr
                     != mico_rtos_create_thread( NULL, MICO_APPLICATION_PRIORITY, "TCP Clients",
                                                 tcp_client_thread,
                                                 0x800, (mico_thread_arg_t)&client_fd  ) )
                    SocketClose( &client_fd  );
            }
        }
    }
    exit:
    if ( err != kNoErr ) wifi_softap_log( "Server listerner thread exit with err: %d", err );
    SocketClose( &tcp_listen_fd );
    mico_rtos_delete_thread( NULL );
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
    wifi_softap_log("uartRecv Thread running!");
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


void micoNotify_WifiStatusHandler(int event, void* arg )
{
  (void)arg;
  switch (event) {
  case NOTIFY_STATION_UP:
    break;
  case NOTIFY_STATION_DOWN:
    break;
  case NOTIFY_AP_UP:
      wifi_softap_log("AP up");
    break;
  case NOTIFY_AP_DOWN:
      wifi_softap_log("AP down");
    break;
  default:
    break;
  }
  return;
}

int application_start( void )
{

    OSStatus err = kNoErr;
    mico_uart_config_t uart_config;
    network_InitTypeDef_st wNetConfig;

    app_context_t* app_context;
    mico_Context_t* mico_context;

    /* Create application context */
    app_context = (app_context_t *) calloc( 1, sizeof(app_context_t) );
    require_action( app_context, exit, err = kNoMemoryErr );

    /* Create mico system context and read application's config data from flash */
    mico_context = mico_system_context_init( sizeof(application_config_t) );
    app_context->appConfig = mico_system_context_get_user_data( mico_context );

    /* mico system initialize */
    err = mico_system_init( mico_context );
    require_noerr( err, exit );

       /* Register user function when wlan connection status is changed */
      err = mico_system_notify_register( mico_notify_WIFI_STATUS_CHANGED, (void *)micoNotify_WifiStatusHandler, NULL );
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


       /*ap*/
       memset(&wNetConfig, 0x0, sizeof(network_InitTypeDef_st));

       strcpy((char*)wNetConfig.wifi_ssid, ap_ssid);
       strcpy((char*)wNetConfig.wifi_key, ap_key);

       wNetConfig.wifi_mode = Soft_AP;
       wNetConfig.dhcpMode = DHCP_Server;
       wNetConfig.wifi_retry_interval = 100;
       strcpy((char*)wNetConfig.local_ip_addr, "192.168.0.1");
       strcpy((char*)wNetConfig.net_mask, "255.255.255.0");
       strcpy((char*)wNetConfig.dnsServer_ip_addr, "192.168.0.1");

       wifi_softap_log("ssid:%s  key:%s", wNetConfig.wifi_ssid, wNetConfig.wifi_key);
       err=micoWlanStart(&wNetConfig);
       wifi_softap_log("err: %d",err);




       err = mico_rtos_create_thread( NULL, MICO_APPLICATION_PRIORITY, "UART Recv", uartRecv_thread,
                                               STACK_SIZE_UART_RECV_THREAD,  (mico_thread_arg_t)app_context );
       require_noerr_string( err, exit, "ERROR: Unable to start the uart recv thread.");


        /* Start TCP server listener thread*/

       err = mico_rtos_create_thread( NULL, MICO_APPLICATION_PRIORITY, "TCP_server", tcp_server_thread,0x400,  (mico_thread_arg_t)app_context  );
       require_noerr_string( err, exit, "ERROR: Unable to start the tcp server thread." );


       exit:
       mico_rtos_delete_thread( NULL );
       return err;
}






