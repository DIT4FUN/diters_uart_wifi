/*
 * server.h
 *
 *  Created on: 2017年9月16日
 *      Author: Aunknownthat
 */

#ifndef DITERS_UART_WIFI_SERVER_SERVER_H_
#define DITERS_UART_WIFI_SERVER_SERVER_H_

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif
#define CONFIGURATION_VERSION               0x00000002 // if default configuration is changed, update this number
#define MAX_QUEUE_NUM                       4  // 1 remote client, 5 local server
#define MAX_QUEUE_LENGTH                    8  // each queue max 8 msg

typedef struct _socket_msg {
  int ref;
  int len;
  uint8_t data[1];
} socket_msg_t;

/*Application's configuration stores in flash*/
typedef struct
{
  uint32_t          configDataVer;
  uint32_t          localServerPort;

  /*local services*/
  bool              localServerEnable;
  bool              remoteServerEnable;
  char              remoteServerDomain[64];
  int               remoteServerPort;

  /*IO settings*/
  uint32_t          USART_BaudRate;
} application_config_t;


/*Running status*/
typedef struct  {
  /*Local clients port list*/
  mico_queue_t*   socket_out_queue[MAX_QUEUE_NUM];
  mico_mutex_t    queue_mtx;
} current_app_status_t;

typedef struct _app_context_t
{
  /*Flash content*/
  application_config_t*     appConfig;

  /*Running status*/
  current_app_status_t      appStatus;
} app_context_t;

#ifdef __cplusplus
} /*extern "C" */
#endif


#endif /* DITERS_UART_WIFI_SERVER_SERVER_H_ */
