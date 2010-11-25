/*
	Onion HTTP server library
	Copyright (C) 2010 David Moreno Montero

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU Affero General Public License as
	published by the Free Software Foundation, either version 3 of the
	License, or (at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU Affero General Public License for more details.

	You should have received a copy of the GNU Affero General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
	*/

#include <malloc.h>
#include <string.h>
#include <stdlib.h>
#include <libgen.h>

#include "onion_server.h"
#include "onion_dict.h"
#include "onion_request.h"
#include "onion_response.h"
#include "onion_handler.h"
#include "onion_server.h"
#include "onion_types_internal.h"
#include "onion_codecs.h"
#include "onion_log.h"


static int onion_request_parse_query(onion_request *req);



/**
 *  @short Creates a request object
 * 
 * @param server onion_server that will be used for writing and some other data
 * @param socket Socket as needed by onion_server write method.
 * @param client_info String that describes the client, for example, the IP address.
 */
onion_request *onion_request_new(onion_server *server, void *socket, const char *client_info){
	onion_request *req;
	req=malloc(sizeof(onion_request));
	memset(req,0,sizeof(onion_request));
	
	req->server=server;
	req->headers=onion_dict_new();
	req->socket=socket;
	req->parse_state=0;
	req->buffer_pos=0;
	req->files=NULL;
	req->post=NULL;
	if (client_info) // This is kept even on clean
		req->client_info=strdup(client_info);
	else
		req->client_info=NULL;

	return req;
}

/// Deletes a request and all its data
void onion_request_free(onion_request *req){
	onion_dict_free(req->headers);
	
	if (req->fullpath)
		free(req->fullpath);
	if (req->query)
		onion_dict_free(req->query);
	if (req->post)
		onion_dict_free(req->post);
	if (req->files)
		onion_dict_free(req->files);

	if (req->client_info)
		free(req->client_info);
	
	free(req);
}

/// Parses the query part to a given dictionary.
static void onion_request_parse_query_to_dict(onion_dict *dict, const char *p){
	char key[32], value[256];
	int state=0;  // 0 key, 1 value
	int i=0;
	while(*p){
		if (state==0){
			if (*p=='='){
				key[i]='\0';
				state=1;
				i=-1;
			}
			else
				key[i]=*p;
		}
		else{
			if (*p=='&'){
				value[i]='\0';
				onion_unquote_inplace(key);
				onion_unquote_inplace(value);
				onion_dict_add(dict, key, value, OD_DUP_ALL);
				state=0;
				i=-1;
			}
			else
				value[i]=*p;
		}
		p++;
		i++;
	}
	if (i!=0 || state!=0){
		if (state==0){
			key[i]='\0';
			value[0]='\0';
		}
		else
			value[i]='\0';
		onion_unquote_inplace(key);
		onion_unquote_inplace(value);
		onion_dict_add(dict, key, value, OD_DUP_ALL);
	}
}

/// Parses the first query line, GET / HTTP/1.1
static int onion_request_fill_query(onion_request *req, const char *data){
	ONION_DEBUG("Request: %s",data);
	char method[16], url[256], version[16];
	sscanf(data,"%15s %255s %15s",method, url, version);
	
	if (strcmp(method,"GET")==0)
		req->flags|=OR_GET;
	else if (strcmp(method,"POST")==0)
		req->flags|=OR_POST;
	else if (strcmp(method,"HEAD")==0)
		req->flags|=OR_HEAD;
	else
		return 0; // Not valid method detected.

	if (strcmp(version,"HTTP/1.1")==0)
		req->flags|=OR_HTTP11;

	req->path=strndup(url,sizeof(url));
	req->fullpath=req->path;
	onion_request_parse_query(req); // maybe it consumes some CPU and not always needed.. but I need the unquotation.
	
	return 1;
}

/// Reads a header and sets it at the request
static int onion_request_fill_header(onion_request *req, const char *data){
	char header[32], value[256];
	sscanf(data, "%31s", header);
	int i=0; 
	const char *p=&data[strlen(header)+1];
	while(*p){
		value[i++]=*p++;
		if (i==sizeof(value)-1){
			break;
		}
	}
	value[i]=0;
	header[strlen(header)-1]='\0'; // removes the :
	onion_dict_add(req->headers, header, value, OD_DUP_ALL);
	return 1;
}

/// Fills the post data.
static int onion_request_fill_post(onion_request *req, const char *data){
	ONION_DEBUG("POST data %s",data);
	req->post=onion_dict_new();
	onion_request_parse_query_to_dict(req->post, data);
	return 1;
}

/**
 * @short Partially fills a request. One line each time.
 * 
 * @returns 0 is error parsing, 1 if ok, -1 if connection should be closed (petition done, close connection).
 */
int onion_request_fill(onion_request *req, const char *data){
	// Internally it uses req->parse_state, states are:
	typedef enum parse_state_e{
		CLEAN=0,
		HEADERS=1,
		POST_DATA=2,
		FINISHED=3,
	}parse_state;

	ONION_DEBUG0("Request: %s",data);

	switch(req->parse_state){
	case CLEAN:
		req->parse_state=HEADERS;
		return onion_request_fill_query(req, data);
	case HEADERS:
		if (data[0]=='\0'){
			if (req->flags&OR_POST){
				req->parse_state=POST_DATA;
				return 1;
			}
			else{
				req->parse_state=FINISHED;
				int s=onion_server_handle_request(req);
				if (s==OR_CLOSE_CONNECTION)
					return -1;
				return 1;
			}
		}
		else
			return onion_request_fill_header(req, data);
	case POST_DATA:
		if (data[0]=='\0'){
			req->parse_state=FINISHED;
			int s=onion_server_handle_request(req);
			if (s==OR_CLOSE_CONNECTION)
				return -1;
			return 1;
		}
		return onion_request_fill_post(req, data);
	case FINISHED:
		ONION_WARNING("Not accepting more data on this status. Clean the request if you want to start a new one.");
	}
	return 0;
}
		/*
	if (!req->path){
		
	}
	else{
	}
	return 1;
}

		if (req->parse_state==HEADERS){
			if (c=='\n'){
				if (req->buffer_pos==0){
					if ((req->flags&(OR_GET|OR_HEAD))!=0)
						req->parse_state=FINISHED;
					else if (req->flags&(OR_POST)){
						req->parse_state=POST_DATA;
						continue;
					}
				}
				else{
					req->buffer[req->buffer_pos]='\0';
					onion_request_fill(req, req->buffer);
					req->buffer_pos=0;
				}
			}
			else{
				if (req->buffer_pos>=sizeof(req->buffer)){ // Overflow on headers
					req->buffer_pos--;
					if (!msgshown){
						ONION_ERROR("Header too long for me (max header length (per header) %d chars). Ignoring from that byte on to the end of this line. (%16s...)",(int) sizeof(req->buffer),req->buffer);
						ONION_ERROR("Increase it at onion_request.h and recompile onion.");
						msgshown=1;
					}
				}
				continue;
			}
		}
		if (req->parse_state==POST_DATA){
			if (c=='\n'){
				if (!req->post)
					req->post=onion_dict_new();
				req->buffer[req->buffer_pos]='\0';
				ONION_DEBUG("POST data %s",req->buffer);
				onion_request_parse_query_to_dict(req->post, req->buffer);
				req->buffer_pos=0;
				req->parse_state=FINISHED;
			}
			else{
				req->buffer[req->buffer_pos]=c;
				req->buffer_pos++;
			}
			
			req->parse_state=FINISHED;
		}
		if (req->parse_state==FINISHED){
			int s=onion_server_handle_request(req);
			if (s==OR_CLOSE_CONNECTION)
				return -i;
			// I do not stop as it might have more data: keep alive.
		}
	}
	return i;
}
*/

/**
 * @short Parses the query to unquote the path and get the query.
 */
static int onion_request_parse_query(onion_request *req){
	if (!req->path)
		return 0;
	if (req->query) // already done
		return 1;
	
	char cleanurl[256];
	int i=0;
	char *p=req->path;
	while(*p){
		if (*p=='?')
			break;
		cleanurl[i++]=*p;
		p++;
	}
	cleanurl[i++]='\0';
	onion_unquote_inplace(cleanurl);
	if (*p){ // There are querys.
		p++;
		req->query=onion_dict_new();
		onion_request_parse_query_to_dict(req->query, p);
	}
	free(req->fullpath);
	req->fullpath=strndup(cleanurl, sizeof(cleanurl));
	req->path=req->fullpath;
	return 1;
}

/**
 * @short Write some data into the request, and passes it line by line to onion_request_fill
 *
 * Just reads line by line and passes the data to onion_request_fill.
 * 
 * Return the number of bytes writen.
 */
int onion_request_write(onion_request *req, const char *data, unsigned int length){
	int i;
	char msgshown=0;
	for (i=0;i<length;i++){
		char c=data[i];
		if (c=='\r') // Just skip it
			continue;
		if (c=='\n'){
			req->buffer[req->buffer_pos]='\0';
			int r=onion_request_fill(req,req->buffer);
			req->buffer_pos=0;
			if (r<=0) // Close connection. Might be a rightfull close, or because of an error. Close anyway.
				return -i;
		}
		else{
			req->buffer[req->buffer_pos]=c;
			req->buffer_pos++;
			if (req->buffer_pos>=sizeof(req->buffer)){ // Overflow on line
				req->buffer_pos--;
				if (!msgshown){
					ONION_ERROR("Read data too long for me (max data length %d chars). Ignoring from that byte on to the end of this line. (%16s...)",(int) sizeof(req->buffer),req->buffer);
					ONION_ERROR("Increase it at onion_request.h and recompile onion.");
					msgshown=1;
				}
			}
		}
	}
	return i;
}


/// Returns a pointer to the string with the current path. Its a const and should not be trusted for long time.
const char *onion_request_get_path(onion_request *req){
	return req->path;
}

/// Moves the pointer inside fullpath to this new position, relative to current path.
void onion_request_advance_path(onion_request *req, int addtopos){
	req->path=&req->path[addtopos];
}

/// Gets a header data
const char *onion_request_get_header(onion_request *req, const char *header){
	return onion_dict_get(req->headers, header);
}

/// Gets a query data
const char *onion_request_get_query(onion_request *req, const char *query){
	if (req->query)
		return onion_dict_get(req->query, query);
	return NULL;
}

/**
 * @short Cleans a request object to reuse it.
 */
void onion_request_clean(onion_request* req){
	onion_dict_free(req->headers);
	req->headers=onion_dict_new();
	req->parse_state=0;
	req->flags&=0xFF00;
	if (req->fullpath){
		free(req->fullpath);
		req->path=req->fullpath=NULL;
	}
	if (req->query){
		onion_dict_free(req->query);
		req->query=NULL;
	}
}

/**
 * @short Forces the request to process only one request, not doing the keep alive.
 * 
 * This is usefull on non threaded modes, as the keep alive blocks the loop.
 */
void onion_request_set_no_keep_alive(onion_request *req){
	req->flags|=OR_NO_KEEP_ALIVE;
	ONION_DEBUG("Disabling keep alive %X",req->flags);
}

/**
 * @short Returns if current request wants to keep alive.
 * 
 * It is a complex set of circumstances: HTTP/1.1 and no connection: close, or HTTP/1.0 and connection: keep-alive
 * and no explicit set that no keep alive.
 */
int onion_request_keep_alive(onion_request *req){
	if (req->flags&OR_NO_KEEP_ALIVE)
		return 0;
	if (req->flags&OR_HTTP11){
		const char *connection=onion_request_get_header(req,"Connection");
		if (!connection || strcasecmp(connection,"Close")!=0) // Other side wants keep alive
			return 1;
	}
	else{ // HTTP/1.0
		const char *connection=onion_request_get_header(req,"Connection");
		if (connection && strcasecmp(connection,"Keep-Alive")==0) // Other side wants keep alive
			return 1;
	}
	return 0;
}
