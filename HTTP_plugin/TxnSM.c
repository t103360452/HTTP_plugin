/** @file
  A brief file description
  @section license License
  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at
      http://www.apache.org/licenses/LICENSE-2.0
  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
 */

#include <sys/types.h>
#include <netinet/in.h>
#include "ts/ink_defs.h"

#ifndef TXN_SM_H
#define TXN_SM_H

typedef int (*TxnSMHandler)(TSCont contp, TSEvent event, void *data);

TSCont TxnSMCreate(TSMutex pmutex, TSVConn client_vc, int server_port);

#define MAX_FILE_PATH_LENGTH 1024

#define TXN_SM_ALIVE 0xAAAA0123
#define TXN_SM_DEAD 0xFEE1DEAD
#define TXN_SM_ZERO 0x00001111

/* The Txn State Machine */
typedef struct _TxnSM {
  unsigned int q_magic;

  TSMutex q_mutex;
  TSAction q_pending_action;
  TxnSMHandler q_current_handler;

  TSVConn q_client_vc;
  TSVConn q_server_vc;

  char *q_client_request;
  char *q_server_response;

  char *q_file_name;
  TSCacheKey q_key;

  char *q_server_name;
  int q_server_port;

  TSVIO q_client_read_vio;
  TSVIO q_client_write_vio;
  TSIOBuffer q_client_request_buffer;
  TSIOBuffer q_client_response_buffer;
  TSIOBufferReader q_client_request_buffer_reader;
  TSIOBufferReader q_client_response_buffer_reader;

  TSVIO q_server_read_vio;
  TSVIO q_server_write_vio;
  TSIOBuffer q_server_request_buffer;
  TSIOBuffer q_server_response_buffer;
  TSIOBufferReader q_server_request_buffer_reader;
  int q_server_response_length;
  int q_block_bytes_read;
  int q_cache_response_length;

  /* Cache related */
  TSVConn q_cache_vc;
  TSIOBufferReader q_cache_response_buffer_reader;
  TSVIO q_cache_read_vio;
  TSVIO q_cache_write_vio;
  TSIOBuffer q_cache_read_buffer;
  TSIOBufferReader q_cache_read_buffer_reader;

} TxnSM;

#endif /* Txn_SM_H */

extern TSTextLogObject protocol_plugin_log;

/* Fix me: currently, tunnelling server_response from OS to both cache and
   client doesn't work for client_vc. So write data first to cache and then
   write cached data to client. */

/* static functions */
int main_handler(TSCont contp, TSEvent event, void *data);

/* functions for clients */
int state_start(TSCont contp, TSEvent event, void *data);
int state_interface_with_client(TSCont contp, TSEvent event, TSVIO vio);
int state_read_request_from_client(TSCont contp, TSEvent event, TSVIO vio);
int state_send_response_to_client(TSCont contp, TSEvent event, TSVIO vio);

/* functions for cache operation */
int state_handle_cache_lookup(TSCont contp, TSEvent event, TSVConn vc);
int state_handle_cache_read_response(TSCont contp, TSEvent event, TSVIO vio);
int state_handle_cache_prepare_for_write(TSCont contp, TSEvent event, TSVConn vc);
int state_write_to_cache(TSCont contp, TSEvent event, TSVIO vio);

/* functions for servers */
int state_build_and_send_request(TSCont contp, TSEvent event, void *data);
int state_dns_lookup(TSCont contp, TSEvent event, TSHostLookupResult host_info);
int state_connect_to_server(TSCont contp, TSEvent event, TSVConn vc);
int state_interface_with_server(TSCont contp, TSEvent event, TSVIO vio);
int state_send_request_to_server(TSCont contp, TSEvent event, TSVIO vio);
int state_read_response_from_server(TSCont contp, TSEvent event, TSVIO vio);

/* misc functions */
int state_done(TSCont contp, TSEvent event, TSVIO vio);

int send_response_to_client(TSCont contp);
int prepare_to_die(TSCont contp);

char *get_info_from_buffer(TSIOBufferReader the_reader);
int is_request_end(char *buf);
int parse_request(char *request, char *server_name, char *file_name);
TSCacheKey CacheKeyCreate(char *file_name);

char* get_http_header_field_value(char* http_header,char* header_field_name);
char** get_http_request_info(char* cilent_request);
int get_header_length(char http_response[]);

/* Continuation handler is a function pointer, this function
   is to assign the continuation handler to a specific function. */
int
main_handler(TSCont contp, TSEvent event, void *data)
{
  TxnSM *txn_sm                  = (TxnSM *)TSContDataGet(contp);
  TxnSMHandler q_current_handler = txn_sm->q_current_handler;

  TSDebug("HTTP_plugin", "main_handler (contp %p event %d)", contp, event);

  /* handle common cases errors */
  if (event == TS_EVENT_ERROR) {
    return prepare_to_die(contp);
  }

  if (q_current_handler != (TxnSMHandler)&state_interface_with_server) {
    if (event == TS_EVENT_VCONN_EOS) {
      return prepare_to_die(contp);
    }
  }

  TSDebug("HTTP_plugin", "current_handler (%p)", q_current_handler);

  return (*q_current_handler)(contp, event, data);
}

/* Create the Txn data structure and the continuation for the Txn. */
TSCont
TxnSMCreate(TSMutex pmutex, TSVConn client_vc, int server_port)
{
  TSCont contp;
  TxnSM *txn_sm;

  txn_sm = (TxnSM *)malloc(sizeof(TxnSM));

  txn_sm->q_mutex          = pmutex;
  txn_sm->q_pending_action = NULL;

  /* Txn will use this server port to connect to the origin server. */
  txn_sm->q_server_port = server_port;
  /* The client_vc is returned by TSNetAccept, refer to Protocol.c. */
  txn_sm->q_client_vc = client_vc;
  /* The server_vc will be created if Txn connects to the origin server. */
  txn_sm->q_server_vc = NULL;

  txn_sm->q_client_read_vio               = NULL;
  txn_sm->q_client_write_vio              = NULL;
  txn_sm->q_client_request_buffer         = NULL;
  txn_sm->q_client_response_buffer        = NULL;
  txn_sm->q_client_request_buffer_reader  = NULL;
  txn_sm->q_client_response_buffer_reader = NULL;

  txn_sm->q_server_read_vio              = NULL;
  txn_sm->q_server_write_vio             = NULL;
  txn_sm->q_server_request_buffer        = NULL;
  txn_sm->q_server_response_buffer       = NULL;
  txn_sm->q_server_request_buffer_reader = NULL;

  /* Char buffers to store client request and server response. */
  txn_sm->q_client_request = (char *)malloc(sizeof(char) * (MAX_REQUEST_LENGTH + 1));
  memset(txn_sm->q_client_request, '\0', (sizeof(char) * (MAX_REQUEST_LENGTH + 1)));
  txn_sm->q_server_response          = NULL;
  txn_sm->q_server_response_length   = 0;
  txn_sm->q_block_bytes_read         = 0;
  txn_sm->q_cache_vc                 = NULL;
  txn_sm->q_cache_response_length    = 0;
  txn_sm->q_cache_read_buffer        = NULL;
  txn_sm->q_cache_read_buffer_reader = NULL;

  txn_sm->q_server_name = (char *)malloc(sizeof(char) * (MAX_SERVER_NAME_LENGTH + 1));
  txn_sm->q_file_name   = (char *)malloc(sizeof(char) * (MAX_FILE_NAME_LENGTH + 1));

  txn_sm->q_key   = NULL;
  txn_sm->q_magic = TXN_SM_ALIVE;

  /* Set the current handler to be state_start. */
  set_handler(txn_sm->q_current_handler, &state_start);

  contp = TSContCreate(main_handler, txn_sm->q_mutex);
  TSContDataSet(contp, txn_sm);
  return contp;
}

/* This function starts to read incoming client request data from client_vc */
int
state_start(TSCont contp, TSEvent event ATS_UNUSED, void *data ATS_UNUSED)
{
  TxnSM *txn_sm = (TxnSM *)TSContDataGet(contp);

  if (!txn_sm->q_client_vc) {
    return prepare_to_die(contp);
  }

  txn_sm->q_client_request_buffer        = TSIOBufferCreate();
  txn_sm->q_client_request_buffer_reader = TSIOBufferReaderAlloc(txn_sm->q_client_request_buffer);

  if (!txn_sm->q_client_request_buffer || !txn_sm->q_client_request_buffer_reader) {
    return prepare_to_die(contp);
  }

  /* Now the IOBuffer and IOBufferReader is ready, the data from
     client_vc can be read into the IOBuffer. Since we don't know
     the size of the client request, set the expecting size to be
     INT64_MAX, so that we will always get TS_EVENT_VCONN_READ_READY
     event, but never TS_EVENT_VCONN_READ_COMPLETE event. */
  set_handler(txn_sm->q_current_handler, (TxnSMHandler)&state_interface_with_client);
  txn_sm->q_client_read_vio = TSVConnRead(txn_sm->q_client_vc, (TSCont)contp, txn_sm->q_client_request_buffer, INT64_MAX);

  return TS_SUCCESS;
}

/* This function is to call proper functions according to the
   VIO argument. If it's read_vio, which means reading request from
   client_vc, call state_read_request_from_client. If it's write_vio,
   which means sending response to client_vc, call
   state_send_response_to_client. If the event is TS_EVENT_VCONN_EOS,
   which means the client closed socket and thus implies the client
   drop all jobs between TxnSM and the client, so go to die. */
int
state_interface_with_client(TSCont contp, TSEvent event, TSVIO vio)
{
  TxnSM *txn_sm = (TxnSM *)TSContDataGet(contp);

  TSDebug("HTTP_plugin", "enter state_interface_with_client");

  txn_sm->q_pending_action = NULL;

  if (vio == txn_sm->q_client_read_vio)
    return state_read_request_from_client(contp, event, vio);

  /* vio == txn_sm->q_client_write_vio */
  return state_send_response_to_client(contp, event, vio);
}

/* Data is read from client_vc, if all data for the request is in,
   parse it and do cache lookup. */
int
state_read_request_from_client(TSCont contp, TSEvent event, TSVIO vio ATS_UNUSED)
{
	int bytes_read ;
	char *temp_buf;
	char **parsed_http_request;
	
  TxnSM *txn_sm = (TxnSM *)TSContDataGet(contp);	//從contp讀取transaction state machine的狀態

  TSDebug("HTTP_plugin", "enter state_read_request_from_client");	//利用TSDebug紀錄log資料

  switch (event) {	//選擇目前event
  case TS_EVENT_VCONN_READ_READY:	//開始從client連線讀取request data ,VCONN_READ_READY (VCONN = VConnect 是一種TCP Socket連線)
    bytes_read = TSIOBufferReaderAvail(txn_sm->q_client_request_buffer_reader);	//讀取client request buffer的資料有多少byte
	int ret_val;
	ret_val = TSTextLogObjectWrite(protocol_plugin_log, "\n\nRead request from client");

	if (ret_val != TS_SUCCESS)
	  TSError("[protocol] Fail to write into log");
    if (bytes_read > 0) {	//bytes_read大於0,表示buffer有效,有資料存在
		FILE *fPtr;
		char *buffer = malloc(1024);
		//txn_sm->q_server_name = "\0";
		fPtr = fopen("/srv/datavol/cdn/host.conf", "r");
		if (fPtr) {
			TSDebug("HTTP_plugin", "*************************************************************************************************************************open file successfully");
			fread(buffer, 1, 1024, fPtr);
			
			fclose(fPtr);
		}
		else {
			TSDebug("HTTP_plugin", "open file failed");
		}
		TSDebug("HTTP_plugin", "1");
		strncpy(txn_sm->q_server_name,buffer,strcspn(buffer,"\n"));
		TSDebug("HTTP_plugin", "2");
		free(buffer);
		TSDebug("HTTP_plugin","%s",txn_sm->q_server_name);
		//↓取得client request buffer的資料，並存成可讀取的char格式。
		temp_buf = (char *)get_info_from_buffer(txn_sm->q_client_request_buffer_reader); 
      	
		parsed_http_request = get_http_request_info(temp_buf);
		txn_sm->q_file_name = parsed_http_request[1];

		int http_request_length = strcspn(temp_buf,"\r");		
		memcpy(txn_sm->q_client_request ,temp_buf, http_request_length+2);
		strncat(txn_sm->q_client_request,"Host: ",6);
		
		strncat(txn_sm->q_client_request,txn_sm->q_server_name,strlen(txn_sm->q_server_name));
		strncat(txn_sm->q_client_request,"\r\nConnection: close\r\n\r\n",25);
		
		TSDebug("HTTP_plugin", "client request file name is %s", txn_sm->q_file_name);
		TSDebug("HTTP_plugin", "client request is %s", txn_sm->q_client_request);
		TSfree(temp_buf);	//釋放temp_buf的記憶體空間
        
		//txn_sm->q_server_name = "dns.ntutee.ml"; 
		
		/* Start to do cache lookup */
        TSDebug("HTTP_plugin", "Key material: file name is %s*****", txn_sm->q_file_name);
		TSDebug("HTTP_plugin", "Key material: server name is %s*****", txn_sm->q_server_name);
        txn_sm->q_key = (TSCacheKey)CacheKeyCreate(txn_sm->q_file_name);	//利用q_file_name建立cache key
		
		char *temp=malloc(10240);
		*(temp+0)='\0';
		strcat(temp,"http://");
		strcat(temp,txn_sm->q_server_name);
		strcat(temp,txn_sm->q_file_name);
		ret_val = TSTextLogObjectWrite(protocol_plugin_log, "Request URL is %s",temp);
		free(temp);
		if (ret_val != TS_SUCCESS)
			TSError("[protocol] Fail to write into log");
		
		//↓將txn_sm->q_current_handler改為state_handle_cache_lookup的程式,準備執行state_handle_cache_lookup
        set_handler(txn_sm->q_current_handler, (TxnSMHandler)&state_handle_cache_lookup);	
        txn_sm->q_pending_action = TSCacheRead(contp, txn_sm->q_key);

        return TS_SUCCESS;
    }

    /* The request is not fully read, reenable the read_vio. */
    TSVIOReenable(txn_sm->q_client_read_vio);
    break;

  default: /* Shouldn't get here, prepare to die. */
    return prepare_to_die(contp);
  }
  return TS_SUCCESS;
}

/* This function handle the cache lookup result. If MISS, try to
   open cache write_vc for writing. Otherwise, use the vc returned
   by the cache to read the data from the cache. */
int
state_handle_cache_lookup(TSCont contp, TSEvent event, TSVConn vc)
{
  TxnSM *txn_sm = (TxnSM *)TSContDataGet(contp);
  int64_t response_size;
  int ret_val;

  TSDebug("HTTP_plugin", "enter state_handle_cache_lookup");

  switch (event) {
  case TS_EVENT_CACHE_OPEN_READ:	//When cache hit
    TSDebug("HTTP_plugin", "cache hit!!!");
    /* Cache hit. */

    /* Write log */
    ret_val = TSTextLogObjectWrite(protocol_plugin_log, "cache hit!!!");
    if (ret_val != TS_SUCCESS)
      TSError("[protocol] Fail to write into log");

    txn_sm->q_cache_vc       = vc;
    txn_sm->q_pending_action = NULL;

    /* Get the size of the cached doc. */
    response_size = TSVConnCacheObjectSizeGet(txn_sm->q_cache_vc);

    /* Allocate IOBuffer to store data from the cache. */
    txn_sm->q_client_response_buffer        = TSIOBufferCreate();
    txn_sm->q_client_response_buffer_reader = TSIOBufferReaderAlloc(txn_sm->q_client_response_buffer);
    txn_sm->q_cache_read_buffer             = TSIOBufferCreate();
    txn_sm->q_cache_read_buffer_reader      = TSIOBufferReaderAlloc(txn_sm->q_cache_read_buffer);

    if (!txn_sm->q_client_response_buffer || !txn_sm->q_client_response_buffer_reader || !txn_sm->q_cache_read_buffer ||
        !txn_sm->q_cache_read_buffer_reader) {
      return prepare_to_die(contp);
    }

    /* Read doc from the cache. */
    set_handler(txn_sm->q_current_handler, (TxnSMHandler)&state_handle_cache_read_response);
    txn_sm->q_cache_read_vio = TSVConnRead(txn_sm->q_cache_vc, contp, txn_sm->q_cache_read_buffer, response_size);

    break;

  case TS_EVENT_CACHE_OPEN_READ_FAILED:
    /* Cache miss or error, open cache write_vc. */
    TSDebug("HTTP_plugin", "cache miss or error!!!");
    /* Write log */
    ret_val = TSTextLogObjectWrite(protocol_plugin_log, "cache miss or error!!!");

    if (ret_val != TS_SUCCESS)
      TSError("[protocol] Fail to write into log");

    set_handler(txn_sm->q_current_handler, (TxnSMHandler)&state_handle_cache_prepare_for_write);
    txn_sm->q_pending_action = TSCacheWrite(contp, txn_sm->q_key);
    break;

  default:
    /* unknown event, abort transaction */
    return prepare_to_die(contp);
  }

  return TS_SUCCESS;
}

static void
load_buffer_cache_data(TxnSM *txn_sm)
{
  /* transfer the data from the cache buffer (which must
     fully be consumed on a VCONN_READY event, to the
     server response buffer */
  int rdr_avail = TSIOBufferReaderAvail(txn_sm->q_cache_read_buffer_reader);

  TSDebug("HTTP_plugin", "entering buffer_cache_data");
  TSDebug("HTTP_plugin", "loading %d bytes to buffer reader", rdr_avail);

  TSAssert(rdr_avail > 0);

  TSIOBufferCopy(txn_sm->q_client_response_buffer,   /* (cache response buffer) */
                 txn_sm->q_cache_read_buffer_reader, /* (transient buffer)      */
                 rdr_avail, 0);

  TSIOBufferReaderConsume(txn_sm->q_cache_read_buffer_reader, rdr_avail);
}

/* If the document is fully read out of the cache, close the
   cache read_vc, send the document to the client. Otherwise,
   reenable the read_vio to read more data out. If some error
   occurs, close the read_vc, open write_vc for writing the doc
   into the cache.*/
int
state_handle_cache_read_response(TSCont contp, TSEvent event, TSVIO vio ATS_UNUSED)
{
	int ret_val;
  TxnSM *txn_sm = (TxnSM *)TSContDataGet(contp);

  TSDebug("HTTP_plugin", "enter state_handle_cache_read_response");

  txn_sm->q_pending_action = NULL;

  switch (event) {
  case TS_EVENT_VCONN_READ_COMPLETE:
  
	
	ret_val = TSTextLogObjectWrite(protocol_plugin_log, "Read file from cache");
    if (ret_val != TS_SUCCESS)
      TSError("[protocol] Fail to write into log");
  
    load_buffer_cache_data(txn_sm);
    TSVConnClose(txn_sm->q_cache_vc);
    txn_sm->q_cache_vc        = NULL;
    txn_sm->q_cache_read_vio  = NULL;
    txn_sm->q_cache_write_vio = NULL;
    TSIOBufferReaderFree(txn_sm->q_cache_read_buffer_reader);
    TSIOBufferDestroy(txn_sm->q_cache_read_buffer);
    txn_sm->q_cache_read_buffer_reader = NULL;
    txn_sm->q_cache_read_buffer        = NULL;
    return send_response_to_client(contp);

  case TS_EVENT_VCONN_READ_READY:
    load_buffer_cache_data(txn_sm);

    TSVIOReenable(txn_sm->q_cache_read_vio);
    break;

  default:
    /* Error */
    if (txn_sm->q_cache_vc) {
      TSVConnClose(txn_sm->q_cache_vc);
      txn_sm->q_cache_vc        = NULL;
      txn_sm->q_cache_read_vio  = NULL;
      txn_sm->q_cache_write_vio = NULL;
    }

    /* Open the write_vc, after getting doc from the origin server,
       write the doc into the cache. */
    set_handler(txn_sm->q_current_handler, (TxnSMHandler)&state_handle_cache_prepare_for_write);
    TSAssert(txn_sm->q_pending_action == NULL);
    txn_sm->q_pending_action = TSCacheWrite(contp, txn_sm->q_key);
    break;
  }
  return TS_SUCCESS;
}

/* The cache processor call us back with the vc to use for writing
   data into the cache.
   In case of error, abort txn. */
int
state_handle_cache_prepare_for_write(TSCont contp, TSEvent event, TSVConn vc)
{
  TxnSM *txn_sm = (TxnSM *)TSContDataGet(contp);

  TSDebug("HTTP_plugin", "enter state_handle_cache_prepare_for_write");

  txn_sm->q_pending_action = NULL;

  switch (event) {
  case TS_EVENT_CACHE_OPEN_WRITE:
    txn_sm->q_cache_vc = vc;
    break;
  default:
    TSError("[protocol] Can't open cache write_vc, aborting txn");
    txn_sm->q_cache_vc = NULL;
    return prepare_to_die(contp);
    break;
  }
  return state_build_and_send_request(contp, 0, NULL);
}

/* Cache miss or error case. Start the process to send the request
   the origin server. */
int
state_build_and_send_request(TSCont contp, TSEvent event ATS_UNUSED, void *data ATS_UNUSED)
{
  TxnSM *txn_sm = (TxnSM *)TSContDataGet(contp);

  TSDebug("HTTP_plugin", "enter state_build_and_send_request");

  txn_sm->q_pending_action = NULL;

  txn_sm->q_server_request_buffer        = TSIOBufferCreate();
  txn_sm->q_server_request_buffer_reader = TSIOBufferReaderAlloc(txn_sm->q_server_request_buffer);

  txn_sm->q_server_response_buffer       = TSIOBufferCreate();
  txn_sm->q_cache_response_buffer_reader = TSIOBufferReaderAlloc(txn_sm->q_server_response_buffer);

  if (!txn_sm->q_server_request_buffer || !txn_sm->q_server_request_buffer_reader || !txn_sm->q_server_response_buffer ||
      !txn_sm->q_cache_response_buffer_reader) {
    return prepare_to_die(contp);
  }

  /* Marshal request */
  TSIOBufferWrite(txn_sm->q_server_request_buffer, txn_sm->q_client_request, strlen(txn_sm->q_client_request));
  TSDebug("HTTP_plugin","server name is %s",txn_sm->q_server_name);
  /* First thing to do is to get the server IP from the server host name. */
  set_handler(txn_sm->q_current_handler, (TxnSMHandler)&state_dns_lookup);
  TSAssert(txn_sm->q_pending_action == NULL);
  txn_sm->q_pending_action = TSHostLookup(contp, txn_sm->q_server_name, strlen(txn_sm->q_server_name));

  TSAssert(txn_sm->q_pending_action);
  TSDebug("HTTP_plugin", "initiating host lookup");

  return TS_SUCCESS;
}

/* If Host lookup is successfully, connect to that IP. */
int
state_dns_lookup(TSCont contp, TSEvent event, TSHostLookupResult host_info)
{
  TxnSM *txn_sm = (TxnSM *)TSContDataGet(contp);
  struct sockaddr const *q_server_addr;
  struct sockaddr_in ip_addr;

  TSDebug("HTTP_plugin", "enter state_dns_lookup");
  
    int ret_val = TSTextLogObjectWrite(protocol_plugin_log, "Connect and send request to original server");

    if (ret_val != TS_SUCCESS)
      TSError("[protocol] Fail to write into log");
  
  /* Can't find the server IP. */
  if (event != TS_EVENT_HOST_LOOKUP || !host_info) {
    return prepare_to_die(contp);
  }
  txn_sm->q_pending_action = NULL;

  /* Get the server IP from data structure TSHostLookupResult. */
  q_server_addr = TSHostLookupResultAddrGet(host_info);

  /* Connect to the server using its IP. */
  //set_handler(txn_sm->q_current_handler, (TxnSMHandler)&state_connect_to_server);
  TSAssert(txn_sm->q_pending_action == NULL);
  TSAssert(q_server_addr->sa_family == AF_INET); /* NO IPv6 in this plugin */
  
  memcpy(&ip_addr, q_server_addr, sizeof(ip_addr));
  TSDebug("HTTP_plugin", "enter server port is %d",txn_sm->q_server_port);
  ip_addr.sin_port         = htons(txn_sm->q_server_port);
  TSDebug("HTTP_plugin", "IP is %d",ip_addr.sin_addr.s_addr);
  
  //custom
  int sockfd, n;
  sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0)
		TSDebug("HTTP_plugin", "ERROR opening socket");
	
	if (connect(sockfd,(struct sockaddr const *)&ip_addr,sizeof(ip_addr)) < 0) 
        TSDebug("HTTP_plugin", "ERROR connecting");
	
	n = write(sockfd,txn_sm->q_client_request,strlen(txn_sm->q_client_request));
	
    if (n < 0) 
        TSDebug("HTTP_plugin", "ERROR writing to socket");
	
	char buffer[1024]; 
	char *http_header_end_ptr=NULL;
    char http_response[104857600]; 
	int bytes_read=0;
    char *content_length_ptr = NULL; 
	int remain_bytes;
    n = 1;
	
    ret_val = TSTextLogObjectWrite(protocol_plugin_log, "Download file from original server");

    if (ret_val != TS_SUCCESS)
      TSError("[protocol] Fail to write into log");
  
    while(n>0 && !http_header_end_ptr){
    	bzero(buffer,1024);
    	n = read(sockfd,buffer,1024);
		memcpy(http_response+bytes_read,buffer,n);
		bytes_read = bytes_read + n;    	
    	http_header_end_ptr = strstr(buffer,"\r\n\r\n");
		
    }
			
    if(http_header_end_ptr != NULL)
	{
		int header_length = get_header_length(http_response);
		
        content_length_ptr = strstr(http_response,"Content-Length");
		char *content_length_temp = malloc(sizeof(char)*11);
		memcpy(content_length_temp,content_length_ptr+16,strcspn(content_length_ptr,"\r")-16);
		int content_length = atoi(content_length_temp);
		free(content_length_temp);
		int all_response_size = content_length + header_length;
		
		remain_bytes = content_length - strlen(http_header_end_ptr+4);
		TSDebug("HTTP_plugin","content_length is %d",content_length);
		
		int content_bytes_read = strlen(http_header_end_ptr+4);
		do{
			bzero(buffer,1024);
			n = read(sockfd,buffer,1024);
			memcpy(http_response+bytes_read,buffer,n);
			bytes_read = bytes_read + n;
			content_bytes_read = content_bytes_read + strlen(buffer);
			remain_bytes = remain_bytes - strlen(buffer);
			TSDebug("HTTP_plugin","\n remain_bytes is %d,bytes_read is %d,content_bytes_read is %d,n is %d",remain_bytes,bytes_read,content_bytes_read,n);
		}while(n > 0);
		
		close(sockfd);
		int bytes_size;
		TSIOBufferWrite(txn_sm->q_server_response_buffer, http_response, all_response_size);
		bytes_size = TSIOBufferReaderAvail(txn_sm->q_cache_response_buffer_reader);
		TSDebug("HTTP_plugin","all_response_size is %d , bytes_size is %d",all_response_size,bytes_size);
		set_handler(txn_sm->q_current_handler, (TxnSMHandler)&state_interface_with_server);
		txn_sm->q_cache_write_vio = TSVConnWrite(txn_sm->q_cache_vc, contp, txn_sm->q_cache_response_buffer_reader, bytes_size);

		ret_val = TSTextLogObjectWrite(protocol_plugin_log, "Save file to cache");

		if (ret_val != TS_SUCCESS)
		  TSError("[protocol] Fail to write into log");
	}
    
	TSDebug("HTTP_plugin", "end");
    
  //custom end
  
  //txn_sm->q_pending_action = TSNetConnect(contp, (struct sockaddr const *)&ip_addr);

  return TS_SUCCESS;
}

/* Net Processor calls back, if succeeded, the net_vc is returned.
   Note here, even if the event is TS_EVENT_NET_CONNECT, it doesn't
   mean the net connection is set up because TSNetConnect is non-blocking.
   Do VConnWrite to the net_vc, if fails, that means there is no net
   connection. */
int
state_connect_to_server(TSCont contp, TSEvent event, TSVConn vc)
{
  TxnSM *txn_sm = (TxnSM *)TSContDataGet(contp);

  TSDebug("HTTP_plugin", "enter state_connect_to_server");

  /* TSNetConnect failed. */
  if (event != TS_EVENT_NET_CONNECT) {
    return prepare_to_die(contp);
  }
  txn_sm->q_pending_action = NULL;

  txn_sm->q_server_vc = vc;

  /* server_vc will be used to write request and read response. */
  set_handler(txn_sm->q_current_handler, (TxnSMHandler)&state_send_request_to_server);
  TSDebug("HTTP_plugin", "enter set_handler");
  TSDebug("HTTP_plugin", "client_request is %s",txn_sm->q_client_request);
  /* Actively write the request to the net_vc. */
  txn_sm->q_server_write_vio =
    TSVConnWrite(txn_sm->q_server_vc, contp, txn_sm->q_server_request_buffer_reader, strlen(txn_sm->q_client_request));
	TSDebug("HTTP_plugin", "enter TSVConnWrite");
  return TS_SUCCESS;
}

/* Net Processor calls back, if write complete, wait for the response
   coming in, otherwise, reenable the write_vio. */
int
state_send_request_to_server(TSCont contp, TSEvent event, TSVIO vio)
{
  TxnSM *txn_sm = (TxnSM *)TSContDataGet(contp);

  TSDebug("HTTP_plugin", "enter state_send_request_to_server");

  switch (event) {
  case TS_EVENT_VCONN_WRITE_READY:
	TSDebug("HTTP_plugin", "enter TS_EVENT_VCONN_WRITE_READY");
    TSVIOReenable(vio);
    break;
  case TS_EVENT_VCONN_WRITE_COMPLETE:
    vio = NULL;
	TSDebug("HTTP_plugin", "enter TS_EVENT_VCONN_WRITE_COMPLETE");
    /* Waiting for the incoming response. */
    set_handler(txn_sm->q_current_handler, (TxnSMHandler)&state_interface_with_server);
    txn_sm->q_server_read_vio = TSVConnRead(txn_sm->q_server_vc, contp, txn_sm->q_server_response_buffer, INT64_MAX);
    break;

  /* it could be failure of TSNetConnect */
  default:
    return prepare_to_die(contp);
  }
  return TS_SUCCESS;
}

/* Call correct handler according to the vio type. */
int
state_interface_with_server(TSCont contp, TSEvent event, TSVIO vio)
{
  TxnSM *txn_sm = (TxnSM *)TSContDataGet(contp);

  TSDebug("HTTP_plugin", "enter state_interface_with_server");

  txn_sm->q_pending_action = NULL;

  switch (event) {
	  
  /* This is returned from cache_vc. */
  case TS_EVENT_VCONN_WRITE_READY:
  
  case TS_EVENT_VCONN_WRITE_COMPLETE:

    return state_write_to_cache(contp, event, vio);
	
  /* Otherwise, handle events from server. */
  case TS_EVENT_VCONN_READ_READY:
  
  /* Actually, we shouldn't get READ_COMPLETE because we set bytes
     count to be INT64_MAX. */
  case TS_EVENT_VCONN_READ_COMPLETE:
    return state_read_response_from_server(contp, event, vio);

  /* all data of the response come in. */
  case TS_EVENT_VCONN_EOS:
    TSDebug("HTTP_plugin", "get server eos");
    /* There is no more use of server_vc, close it. */
    if (txn_sm->q_server_vc) {
      TSVConnClose(txn_sm->q_server_vc);
      txn_sm->q_server_vc = NULL;
    }
    txn_sm->q_server_read_vio  = NULL;
    txn_sm->q_server_write_vio = NULL;

    /* Check if the response is good */
    if (txn_sm->q_server_response_length == 0) {
      /* This is the bad response. Close client_vc. */
      if (txn_sm->q_client_vc) {
        TSVConnClose(txn_sm->q_client_vc);
        txn_sm->q_client_vc = NULL;
      }
      txn_sm->q_client_read_vio  = NULL;
      txn_sm->q_client_write_vio = NULL;

      /* Close cache_vc as well. */
      if (txn_sm->q_cache_vc) {
        TSVConnClose(txn_sm->q_cache_vc);
        txn_sm->q_cache_vc = NULL;
      }
      txn_sm->q_cache_write_vio = NULL;
      return state_done(contp, 0, NULL);
    }

    if (txn_sm->q_cache_response_length >= txn_sm->q_server_response_length) {
      /* Write is complete, close the cache_vc. */
      TSVConnClose(txn_sm->q_cache_vc);
      txn_sm->q_cache_vc        = NULL;
      txn_sm->q_cache_write_vio = NULL;
      TSIOBufferReaderFree(txn_sm->q_cache_response_buffer_reader);

      /* Open cache_vc to read data and send to client. */
      set_handler(txn_sm->q_current_handler, (TxnSMHandler)&state_handle_cache_lookup);
      txn_sm->q_pending_action = TSCacheRead(contp, txn_sm->q_key);
    } else { /* not done with writing into cache */

      TSDebug("HTTP_plugin", "cache_response_length is %d, server response length is %d", txn_sm->q_cache_response_length,
              txn_sm->q_server_response_length);
      TSVIOReenable(txn_sm->q_cache_write_vio);
    }

  default:
    break;
  }

  return TS_SUCCESS;
}

/* The response comes in. If the origin server finishes writing, it
   will close the socket, so the event returned from the net_vc is
   TS_EVENT_VCONN_EOS. By this event, TxnSM knows all data of the
   response arrives and so parse it, save a copy in the cache and
   send the doc to the client. If reading is not done, reenable the
   read_vio. */
int
state_read_response_from_server(TSCont contp, TSEvent event ATS_UNUSED, TSVIO vio ATS_UNUSED)
{
  TxnSM *txn_sm  = (TxnSM *)TSContDataGet(contp);
  int bytes_read = 0;

  TSDebug("HTTP_plugin", "enter state_read_response_from_server");

  bytes_read = TSIOBufferReaderAvail(txn_sm->q_cache_response_buffer_reader);

  if ((bytes_read > 0) && (txn_sm->q_cache_vc)) {
    /* If this is the first write, do TSVConnWrite, otherwise, simply
       reenable q_cache_write_vio. */
    if (txn_sm->q_server_response_length == 0) {
		//bytes_read=1024;
		TSDebug("HTTP_plugin", "q_server_response_length is zero");
		set_handler(txn_sm->q_current_handler, (TxnSMHandler)&state_interface_with_server);
		txn_sm->q_cache_write_vio = TSVConnWrite(txn_sm->q_cache_vc, contp, txn_sm->q_cache_response_buffer_reader, bytes_read);
    } else {
		TSDebug("HTTP_plugin", "q_server_response_length is not zero");
		TSAssert(txn_sm->q_server_response_length > 0);
		//TSVIOReenable(txn_sm->q_cache_write_vio);
		txn_sm->q_block_bytes_read = bytes_read;
		set_handler(txn_sm->q_current_handler, (TxnSMHandler)&state_interface_with_server);
		txn_sm->q_cache_write_vio = TSVConnWrite (txn_sm->q_cache_vc,contp,txn_sm->q_cache_response_buffer_reader,bytes_read);
    }
  }

  txn_sm->q_server_response_length += bytes_read;
  TSDebug("HTTP_plugin", "bytes read is %d, total response length is %d", bytes_read, txn_sm->q_server_response_length);

  return TS_SUCCESS;
}

/* If the whole doc has been written into the cache, send the response
   to the client, otherwise, reenable the read_vio. */
int
state_write_to_cache(TSCont contp, TSEvent event, TSVIO vio)
{
  TxnSM *txn_sm = (TxnSM *)TSContDataGet(contp);

  TSDebug("HTTP_plugin", "enter state_write_to_cache");

  switch (event) {
  case TS_EVENT_VCONN_WRITE_READY:
	TSDebug("HTTP_plugin","TS_EVENT_VCONN_WRITE_READY TSVIOReenable(txn_sm->q_cache_write_vio);");
    TSVIOReenable(txn_sm->q_cache_write_vio);
    return TS_SUCCESS;

  case TS_EVENT_VCONN_WRITE_COMPLETE:
    TSDebug("HTTP_plugin", "nbytes %" PRId64 ", ndone %" PRId64, TSVIONBytesGet(vio), TSVIONDoneGet(vio));
    /* Since the first write is through TSVConnWrite, which aleady consume
       the data in cache_buffer_reader, don't consume it again. */
    if (txn_sm->q_cache_response_length > 0 && txn_sm->q_block_bytes_read > 0){
		TSDebug("HTTP_plugin", "TSIOBufferReaderConsume(txn_sm->q_cache_response_buffer_reader, txn_sm->q_block_bytes_read);");
		TSIOBufferReaderConsume(txn_sm->q_cache_response_buffer_reader, txn_sm->q_block_bytes_read);
	}

    txn_sm->q_cache_response_length += TSVIONBytesGet(vio);
	
	/*TSVConnClose(txn_sm->q_server_vc);
	txn_sm->q_server_vc = NULL;
	txn_sm->q_server_read_vio  = NULL;
    txn_sm->q_server_write_vio = NULL;*/
	
    /* If not all data have been read in, we have to reenable the read_vio */
    if (txn_sm->q_server_vc != NULL) {
      TSDebug("HTTP_plugin", "re-enable server_read_vio");
		
		TSVIOReenable(txn_sm->q_server_read_vio);
      return TS_SUCCESS;
    }

    if (txn_sm->q_cache_response_length >= txn_sm->q_server_response_length) {
      /* Write is complete, close the cache_vc. */
      TSDebug("HTTP_plugin", "close cache_vc, cache_response_length is %d, server_response_lenght is %d",
              txn_sm->q_cache_response_length, txn_sm->q_server_response_length);
      TSVConnClose(txn_sm->q_cache_vc);
      txn_sm->q_cache_vc        = NULL;
      txn_sm->q_cache_write_vio = NULL;
      TSIOBufferReaderFree(txn_sm->q_cache_response_buffer_reader);

      /* Open cache_vc to read data and send to client. */
      set_handler(txn_sm->q_current_handler, (TxnSMHandler)&state_handle_cache_lookup);
      txn_sm->q_pending_action = TSCacheRead(contp, txn_sm->q_key);
    } else { /* not done with writing into cache */

      TSDebug("HTTP_plugin", "re-enable cache_write_vio");
      TSVIOReenable(txn_sm->q_cache_write_vio);
    }
    return TS_SUCCESS;
  default:
    break;
  }

  /* Something wrong if getting here. */
  return prepare_to_die(contp);
}

/* If the response has been fully written into the client_vc,
   which means this txn is done, close the client_vc. Otherwise,
   reenable the write_vio. */
int
state_send_response_to_client(TSCont contp, TSEvent event, TSVIO vio)
{
	int ret_val;
  TxnSM *txn_sm = (TxnSM *)TSContDataGet(contp);

  TSDebug("HTTP_plugin", "enter state_send_response_to_client");

  switch (event) {
  case TS_EVENT_VCONN_WRITE_READY:
    TSDebug("HTTP_plugin", " . wr ready");
    TSDebug("HTTP_plugin", "write_ready: nbytes %" PRId64 ", ndone %" PRId64, TSVIONBytesGet(vio), TSVIONDoneGet(vio));
    TSVIOReenable(txn_sm->q_client_write_vio);
    break;

  case TS_EVENT_VCONN_WRITE_COMPLETE:
  
  	
	ret_val = TSTextLogObjectWrite(protocol_plugin_log, "Send file to client");
    if (ret_val != TS_SUCCESS)
      TSError("[protocol] Fail to write into log");
  
    TSDebug("HTTP_plugin", " . wr complete");
    TSDebug("HTTP_plugin", "write_complete: nbytes %" PRId64 ", ndone %" PRId64, TSVIONBytesGet(vio), TSVIONDoneGet(vio));
    /* Finished sending all data to client, close client_vc. */
    if (txn_sm->q_client_vc) {
      TSVConnClose(txn_sm->q_client_vc);
      txn_sm->q_client_vc = NULL;
    }
    txn_sm->q_client_read_vio  = NULL;
    txn_sm->q_client_write_vio = NULL;

    return state_done(contp, 0, NULL);

  default:
    TSDebug("HTTP_plugin", " . default handler");
    return prepare_to_die(contp);
  }

  TSDebug("HTTP_plugin", "leaving send_response_to_client");

  return TS_SUCCESS;
}

/* There is something wrong, abort client, server and cache vc
   if they exist. */
int
prepare_to_die(TSCont contp)
{
  TxnSM *txn_sm = (TxnSM *)TSContDataGet(contp);

  TSDebug("HTTP_plugin", "enter prepare_to_die");
  if (txn_sm->q_client_vc) {
    TSVConnAbort(txn_sm->q_client_vc, 1);
    txn_sm->q_client_vc = NULL;
  }
  txn_sm->q_client_read_vio  = NULL;
  txn_sm->q_client_write_vio = NULL;

  if (txn_sm->q_server_vc) {
    TSVConnAbort(txn_sm->q_server_vc, 1);
    txn_sm->q_server_vc = NULL;
  }
  txn_sm->q_server_read_vio  = NULL;
  txn_sm->q_server_write_vio = NULL;

  if (txn_sm->q_cache_vc) {
    TSVConnAbort(txn_sm->q_cache_vc, 1);
    txn_sm->q_cache_vc = NULL;
  }
  txn_sm->q_cache_read_vio  = NULL;
  txn_sm->q_cache_write_vio = NULL;

  return state_done(contp, 0, NULL);
}

int
state_done(TSCont contp, TSEvent event ATS_UNUSED, TSVIO vio ATS_UNUSED)
{
  TxnSM *txn_sm = (TxnSM *)TSContDataGet(contp);

  TSDebug("HTTP_plugin", "enter state_done");

  if (txn_sm->q_pending_action && !TSActionDone(txn_sm->q_pending_action)) {
    TSDebug("HTTP_plugin", "cancelling pending action %p", txn_sm->q_pending_action);
    TSActionCancel(txn_sm->q_pending_action);
  } else if (txn_sm->q_pending_action) {
    TSDebug("HTTP_plugin", "action is done %p", txn_sm->q_pending_action);
  }
TSDebug("HTTP_plugin", "enter state_done  q_pending_action");
  txn_sm->q_pending_action = NULL;
  txn_sm->q_mutex          = NULL;
TSDebug("HTTP_plugin", "enter state_done  q_client_request_buffer");
  if (txn_sm->q_client_request_buffer) {
    if (txn_sm->q_client_request_buffer_reader)
      TSIOBufferReaderFree(txn_sm->q_client_request_buffer_reader);
    TSIOBufferDestroy(txn_sm->q_client_request_buffer);
    txn_sm->q_client_request_buffer        = NULL;
    txn_sm->q_client_request_buffer_reader = NULL;
  }
TSDebug("HTTP_plugin", "enter state_done q_client_response_buffer");
  if (txn_sm->q_client_response_buffer) {
    if (txn_sm->q_client_response_buffer_reader)
      TSIOBufferReaderFree(txn_sm->q_client_response_buffer_reader);

    TSIOBufferDestroy(txn_sm->q_client_response_buffer);
    txn_sm->q_client_response_buffer        = NULL;
    txn_sm->q_client_response_buffer_reader = NULL;
  }
TSDebug("HTTP_plugin", "enter state_done q_cache_read_buffer");
  if (txn_sm->q_cache_read_buffer) {
    if (txn_sm->q_cache_read_buffer_reader)
      TSIOBufferReaderFree(txn_sm->q_cache_read_buffer_reader);
    TSIOBufferDestroy(txn_sm->q_cache_read_buffer);
    txn_sm->q_cache_read_buffer        = NULL;
    txn_sm->q_cache_read_buffer_reader = NULL;
  }
TSDebug("HTTP_plugin", "enter state_done q_server_request_buffer");
  if (txn_sm->q_server_request_buffer) {
    if (txn_sm->q_server_request_buffer_reader)
      TSIOBufferReaderFree(txn_sm->q_server_request_buffer_reader);
    TSIOBufferDestroy(txn_sm->q_server_request_buffer);
    txn_sm->q_server_request_buffer        = NULL;
    txn_sm->q_server_request_buffer_reader = NULL;
  }
TSDebug("HTTP_plugin", "enter state_done q_server_response_buffer");
  if (txn_sm->q_server_response_buffer) {
    TSIOBufferDestroy(txn_sm->q_server_response_buffer);
    txn_sm->q_server_response_buffer = NULL;
  }
TSDebug("HTTP_plugin", "enter state_done q_server_name");
  if (txn_sm->q_server_name) {
	  TSDebug("HTTP_plugin", "enter state_done free q_server_name");
    //free(txn_sm->q_server_name);
	TSDebug("HTTP_plugin", "enter state_done q_server_name null");
    txn_sm->q_server_name = NULL;
  }
TSDebug("HTTP_plugin", "enter state_done q_file_name");
  if (txn_sm->q_file_name) {
    //free(txn_sm->q_file_name);
    txn_sm->q_file_name = NULL;
  }
TSDebug("HTTP_plugin", "enter state_done q_key");
  if (txn_sm->q_key)
    TSCacheKeyDestroy(txn_sm->q_key);
TSDebug("HTTP_plugin", "enter state_done q_client_request");
  if (txn_sm->q_client_request) {
    //free(txn_sm->q_client_request);
    txn_sm->q_client_request = NULL;
  }
TSDebug("HTTP_plugin", "enter state_done q_server_response");
  if (txn_sm->q_server_response) {
    //free(txn_sm->q_server_response);
    txn_sm->q_server_response = NULL;
  }
  TSDebug("HTTP_plugin", "enter state_done txn_sm");
  if (txn_sm) {
    txn_sm->q_magic = TXN_SM_DEAD;
    //free(txn_sm);
  }
  TSDebug("HTTP_plugin", "enter state_done TSContDestroy");
  TSContDestroy(contp);
  TSDebug("HTTP_plugin", "enter state_done TS_EVENT_NONE");
  
  	int ret_val;
	ret_val = TSTextLogObjectWrite(protocol_plugin_log, "Close all connect");
    if (ret_val != TS_SUCCESS)
      TSError("[protocol] Fail to write into log");
  
  return TS_EVENT_NONE;
}

/* Write the data into the client_vc. */
int
send_response_to_client(TSCont contp)
{
  TxnSM *txn_sm;
  int response_len;

  TSDebug("HTTP_plugin", "enter send_response_to_client");

  txn_sm       = (TxnSM *)TSContDataGet(contp);
  response_len = TSIOBufferReaderAvail(txn_sm->q_client_response_buffer_reader);

  TSDebug("HTTP_plugin", " . resp_len is %d", response_len);

  set_handler(txn_sm->q_current_handler, (TxnSMHandler)&state_interface_with_client);
  txn_sm->q_client_write_vio =
    TSVConnWrite(txn_sm->q_client_vc, (TSCont)contp, txn_sm->q_client_response_buffer_reader, response_len);
  return TS_SUCCESS;
}

/* Read data out through the_reader and save it in a char buffer. */
char *
get_info_from_buffer(TSIOBufferReader the_reader)
{
  char *info;
  char *info_start;

  int64_t read_avail, read_done;
  TSIOBufferBlock blk;
  char *buf;

  if (!the_reader)
    return NULL;

  read_avail = TSIOBufferReaderAvail(the_reader);

  info = (char *)malloc(sizeof(char) * read_avail);
  if (info == NULL)
    return NULL;
  info_start = info;

  /* Read the data out of the reader */
  while (read_avail > 0) {
    blk = TSIOBufferReaderStart(the_reader);
    buf = (char *)TSIOBufferBlockReadStart(blk, the_reader, &read_done);
    memcpy(info, buf, read_done);
    if (read_done > 0) {
      TSIOBufferReaderConsume(the_reader, read_done);
      read_avail -= read_done;
      info += read_done;
    }
  }

  return info_start;
}

/* Create 128-bit cache key based on the input string, in this case,
   the file_name of the requested doc. */
TSCacheKey
CacheKeyCreate(char *file_name)
{
  TSCacheKey key;

  /* TSCacheKeyCreate is to allocate memory space for the key */
  key = TSCacheKeyCreate();

  /* TSCacheKeyDigestSet is to compute TSCackeKey from the input string */
  TSCacheKeyDigestSet(key, file_name, strlen(file_name));
  return key;
}

char* get_http_header_field_value(char* cilent_request,char* header_field_name){
	static char http_header_field_value[100] = "";
	char* http_header_ptr = NULL;
	int http_header_field_value_length = 0;
	int http_header_field_name_length = 0;
		
	http_header_ptr = strstr(cilent_request,header_field_name);	//搜尋是否有此欄位,並取得該欄位的指標位置
	if(!http_header_ptr)return NULL;	//如果找不到該欄位,回傳NULL
	
	http_header_field_name_length = strlen(header_field_name) + 2; //要加": "的長度
	http_header_ptr += http_header_field_name_length;	//將http_header_ptr指標位置移到": "的後面
	
	http_header_field_value_length = strcspn(http_header_ptr,"\r");	//取得header_value的長度
	if(http_header_field_value_length == 0)return NULL;	//如果找不到該欄位,回傳NULL
	
	memcpy(http_header_field_value ,http_header_ptr ,http_header_field_value_length);	//取得http_header_field_value的資料
	return http_header_field_value;
}

char** get_http_request_info(char* cilent_request){
	int http_request_length = 0;
	int i=0;
	char http_request[200] = "";
	char *tmp = NULL;
	static char *parsed_http_request[3];
    for(i=0;i<3;i++)
    {
        parsed_http_request[i] = malloc(sizeof(char) * 100);
    }
	http_request_length = strcspn(cilent_request,"\r");
	memcpy(http_request,cilent_request,http_request_length);
	tmp = strtok(http_request," ");
	i=0;
	while (tmp != NULL)
    {
        strcpy(parsed_http_request[i],tmp);
        tmp = strtok (NULL, " ");
        i+=1;
    }
    return parsed_http_request;
}

int get_header_length(char http_response[]){

	char *http_response_ptr = NULL;
	int line_length = 0;
	int total_length = 0;
	
	line_length = strcspn(http_response,"\n")+1;
	total_length += line_length;
	http_response_ptr = strstr(http_response,"\n");
	
	while( strncmp(http_response_ptr+1,"\r\n",2) != 0)//判斷是不是header結尾
	{
		line_length = strcspn(http_response_ptr+1,"\n")+1; 	//\n也要算進去
		total_length += line_length;
		http_response_ptr = strstr(http_response_ptr +1,"\n");  //取得此行最後一個位置的指標
	}
	total_length += 2;//加入結尾(\r\n)
	
	return total_length;
}