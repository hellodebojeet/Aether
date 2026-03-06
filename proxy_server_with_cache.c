#define _GNU_SOURCE
#include "proxy_parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/wait.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <poll.h>

#define MAX_BYTES 4096    //max allowed size of request/response
#define MAX_CLIENTS 400     //max number of client requests served at a time
#define MAX_SIZE 200*(1<<20)     //size of the cache
#define MAX_ELEMENT_SIZE 10*(1<<20)     //max size of an element in cache

typedef struct cache_element cache_element;

struct cache_element{
    char* data;             // data stores response
    int len;                // length of data
    char* url;              // url stores the request (cache key)
    time_t lru_time_track;  // most recent access time
    cache_element* next;    // pointer to next element
    // HTTP caching metadata
    time_t expiry_time;     // absolute unix time after which entry is stale (0 = no expiry)
    char* etag;             // ETag header value (may be NULL)
    char* last_modified;    // Last-Modified header value (may be NULL)
};

cache_element* find(char* url);
int add_cache_element(char* data, int size, char* url, time_t expiry, const char* etag, const char* last_modified);
void remove_cache_element();

int port_number = 8080;				// Default Port
int proxy_socketId;					// socket descriptor of proxy server
pthread_t tid[MAX_CLIENTS];         //array to store the thread ids of clients
sem_t seamaphore;	                //if client requests exceeds the max_clients this seamaphore puts the
                                    //waiting threads to sleep and wakes them when traffic on queue decreases
//sem_t cache_lock;			       
pthread_mutex_t lock;               //lock is used for locking the cache


cache_element* head;                //pointer to the cache
int cache_size;             //cache_size denotes the current size of the cache

/* Metrics and Logging */
unsigned long long metric_total_requests = 0;
unsigned long long metric_active_connections = 0;
unsigned long long metric_cache_hits = 0;
unsigned long long metric_cache_misses = 0;
pthread_mutex_t metrics_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

struct rate_limit_entry {
    char ip[INET_ADDRSTRLEN];
    int count;
    time_t window_start;
};
#define MAX_RL_ENTRIES 1000
struct rate_limit_entry rl_table[MAX_RL_ENTRIES] = {0};
pthread_mutex_t rl_lock = PTHREAD_MUTEX_INITIALIZER;

void log_access(const char* client_ip, const char* method, const char* url, int status_code, int response_bytes, int latency_ms, const char* cache_status) {
    pthread_mutex_lock(&log_lock);
    FILE *f = fopen("access.log", "a");
    if (f) {
        time_t now = time(NULL);
        struct tm *t = gmtime(&now);
        char timebuf[64];
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%dT%H:%M:%SZ", t);
        fprintf(f, "%s %s %s %s %d %d %dms %s\n", timebuf, client_ip, method, url, status_code, response_bytes, latency_ms, cache_status);
        fclose(f);
    }
    pthread_mutex_unlock(&log_lock);
}

void log_error(const char* error_message, const char* upstream_host) {
    pthread_mutex_lock(&log_lock);
    FILE *f = fopen("error.log", "a");
    if (f) {
        time_t now = time(NULL);
        struct tm *t = gmtime(&now);
        char timebuf[64];
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%dT%H:%M:%SZ", t);
        fprintf(f, "%s %s %s\n", timebuf, error_message, upstream_host ? upstream_host : "-");
        fclose(f);
    }
    pthread_mutex_unlock(&log_lock);
}

int is_allowed_ip(const char* ip) {
    if (strcmp(ip, "127.0.0.1") == 0) return 1;
    if (strncmp(ip, "192.168.1.", 10) == 0) return 1;
    return 0; // deny all others
}

int check_rate_limit(const char* ip) {
    time_t now = time(NULL);
    int allowed = 1;
    pthread_mutex_lock(&rl_lock);
    int found_idx = -1;
    int empty_idx = -1;
    for (int i=0; i<MAX_RL_ENTRIES; i++) {
        if (rl_table[i].ip[0] != '\0' && strcmp(rl_table[i].ip, ip) == 0) {
            found_idx = i;
            break;
        }
        if (empty_idx == -1 && rl_table[i].ip[0] == '\0') {
            empty_idx = i;
        }
    }
    int target_idx = (found_idx != -1) ? found_idx : empty_idx;
    if (target_idx != -1) {
        if (target_idx == empty_idx) {
            strcpy(rl_table[target_idx].ip, ip);
            rl_table[target_idx].count = 1;
            rl_table[target_idx].window_start = now;
        } else {
            if (now - rl_table[target_idx].window_start >= 60) {
                rl_table[target_idx].count = 1;
                rl_table[target_idx].window_start = now;
            } else {
                rl_table[target_idx].count++;
                if (rl_table[target_idx].count > 100) allowed = 0;
            }
        }
    }
    pthread_mutex_unlock(&rl_lock);
    return allowed;
}

int sendErrorMessage(int socket, int status_code)
{
	char str[1024];
	char currentTime[50];
	time_t now = time(0);

	struct tm data = *gmtime(&now);
	strftime(currentTime,sizeof(currentTime),"%a, %d %b %Y %H:%M:%S %Z", &data);

	switch(status_code)
	{
		case 400: snprintf(str, sizeof(str), "HTTP/1.1 400 Bad Request\r\nContent-Length: 95\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>400 Bad Request</TITLE></HEAD>\n<BODY><H1>400 Bad Request</H1>\n</BODY></HTML>", currentTime);
				  printf("400 Bad Request\n");
				  send(socket, str, strlen(str), 0);
				  break;

		case 403: snprintf(str, sizeof(str), "HTTP/1.1 403 Forbidden\r\nContent-Length: 112\r\nContent-Type: text/html\r\nConnection: keep-alive\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>403 Forbidden</TITLE></HEAD>\n<BODY><H1>403 Forbidden</H1><br>Permission Denied\n</BODY></HTML>", currentTime);
				  printf("403 Forbidden\n");
				  send(socket, str, strlen(str), 0);
				  break;

		case 404: snprintf(str, sizeof(str), "HTTP/1.1 404 Not Found\r\nContent-Length: 91\r\nContent-Type: text/html\r\nConnection: keep-alive\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>404 Not Found</TITLE></HEAD>\n<BODY><H1>404 Not Found</H1>\n</BODY></HTML>", currentTime);
				  printf("404 Not Found\n");
				  send(socket, str, strlen(str), 0);
				  break;

		case 500: snprintf(str, sizeof(str), "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 115\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>500 Internal Server Error</TITLE></HEAD>\n<BODY><H1>500 Internal Server Error</H1>\n</BODY></HTML>", currentTime);
				  //printf("500 Internal Server Error\n");
				  send(socket, str, strlen(str), 0);
				  break;

		case 501: snprintf(str, sizeof(str), "HTTP/1.1 501 Not Implemented\r\nContent-Length: 103\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>404 Not Implemented</TITLE></HEAD>\n<BODY><H1>501 Not Implemented</H1>\n</BODY></HTML>", currentTime);
				  printf("501 Not Implemented\n");
				  send(socket, str, strlen(str), 0);
				  break;

		case 505: snprintf(str, sizeof(str), "HTTP/1.1 505 HTTP Version Not Supported\r\nContent-Length: 125\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>505 HTTP Version Not Supported</TITLE></HEAD>\n<BODY><H1>505 HTTP Version Not Supported</H1>\n</BODY></HTML>", currentTime);
				  printf("505 HTTP Version Not Supported\n");
				  send(socket, str, strlen(str), 0);
				  break;

		default:  return -1;

	}
	return 1;
}

int connectRemoteServer(char* host_addr, int port_num)
{
	// Creating Socket for remote server ---------------------------

	int remoteSocket = socket(AF_INET, SOCK_STREAM, 0);

	if( remoteSocket < 0)
	{
		printf("Error in Creating Socket.\n");
		return -1;
	}
	
	// Get host by the name or ip address provided

	struct hostent *host = gethostbyname(host_addr);	
	if(host == NULL)
	{
		fprintf(stderr, "No such host exists.\n");	
		return -1;
	}

	// inserts ip address and port number of host in struct `server_addr`
	struct sockaddr_in server_addr;

	bzero((char*)&server_addr, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(port_num);

	bcopy((char *)host->h_addr,(char *)&server_addr.sin_addr.s_addr,host->h_length);

	// Connect to Remote server ----------------------------------------------------

	if( connect(remoteSocket, (struct sockaddr*)&server_addr, (socklen_t)sizeof(server_addr)) < 0 )
	{
		fprintf(stderr, "Error in connecting !\n"); 
		return -1;
	}
	// free(host_addr);
	return remoteSocket;
}

void tunnel_data(int client_fd, int remote_fd) {
    struct pollfd fds[2];
    fds[0].fd = client_fd;
    fds[0].events = POLLIN;
    fds[1].fd = remote_fd;
    fds[1].events = POLLIN;
    
    char buffer[8192];
    while (1) {
        int ret = poll(fds, 2, -1);
        if (ret < 0) {
            break;
        }
        
        if (fds[0].revents & POLLIN) {
            int bytes = recv(client_fd, buffer, sizeof(buffer), 0);
            if (bytes <= 0) break;
            int sent = send(remote_fd, buffer, bytes, 0);
            if (sent <= 0) break;
        }
        if (fds[1].revents & POLLIN) {
            int bytes = recv(remote_fd, buffer, sizeof(buffer), 0);
            if (bytes <= 0) break;
            int sent = send(client_fd, buffer, bytes, 0);
            if (sent <= 0) break;
        }
        if ((fds[0].revents & (POLLERR | POLLHUP)) ||
            (fds[1].revents & (POLLERR | POLLHUP))) {
            break;
        }
    }
}

int handle_request(int clientSocket, ParsedRequest *request, char *tempReq,
                   const char *cached_etag, const char *cached_last_modified)
{
	char *buf = (char*)malloc(sizeof(char)*MAX_BYTES);
	strcpy(buf, "GET ");
	strcat(buf, request->path);
	strcat(buf, " ");
	strcat(buf, request->version);
	strcat(buf, "\r\n");

	size_t len = strlen(buf);

	if (ParsedHeader_set(request, "Connection", "close") < 0){
		printf("set header key not work\n");
	}

	if(ParsedHeader_get(request, "Host") == NULL)
	{
		if(ParsedHeader_set(request, "Host", request->host) < 0){
			printf("Set \"Host\" header key not working\n");
		}
	}

    // Inject conditional GET headers if we have cached validators
    if (cached_etag) {
        ParsedHeader_set(request, "If-None-Match", cached_etag);
    }
    if (cached_last_modified) {
        ParsedHeader_set(request, "If-Modified-Since", cached_last_modified);
    }

	if (ParsedRequest_unparse_headers(request, buf + len, (size_t)MAX_BYTES - len) < 0) {
		printf("unparse failed\n");
	}

	int server_port = 80;
	if(request->port != NULL)
		server_port = atoi(request->port);

	int remoteSocketID = connectRemoteServer(request->host, server_port);
	if(remoteSocketID < 0) return -1;
	int bytes_send = send(remoteSocketID, buf, strlen(buf), 0);
	bzero(buf, MAX_BYTES);

	char *temp_buffer = (char*)malloc(sizeof(char)*MAX_BYTES);
	int temp_buffer_size = MAX_BYTES;
	int temp_buffer_index = 0;
    
    int header_done = 0;
    long long content_length = -1;
    long long body_bytes_read = 0;
    int is_chunked = 0;
    int cacheable = 1;
    int response_status_code = 0;
    // HTTP caching metadata extracted from response
    char resp_etag[512] = {0};
    char resp_last_modified[512] = {0};
    time_t resp_expiry = 0;

	while (1) {
		bytes_send = recv(remoteSocketID, buf, MAX_BYTES-1, 0);
        if (bytes_send <= 0) break;
        
		send(clientSocket, buf, bytes_send, 0);
		
        if (!header_done || cacheable) {
            for(int i=0; i<bytes_send; i++) {
                temp_buffer[temp_buffer_index++] = buf[i];
                if (temp_buffer_index >= temp_buffer_size) {
                    temp_buffer_size += MAX_BYTES;
                    temp_buffer = (char*)realloc(temp_buffer, temp_buffer_size);
                }
            }
        } else if (is_chunked) {
            // Not cacheable, but chunked: only keep last 5 bytes
            for(int i=0; i<bytes_send; i++) {
                temp_buffer[temp_buffer_index++] = buf[i];
                if (temp_buffer_index > 10) {
                    memmove(temp_buffer, temp_buffer + temp_buffer_index - 5, 5);
                    temp_buffer_index = 5;
                }
            }
        }

        if (!header_done) {
            char *hdr_end = strstr(temp_buffer, "\r\n\r\n");
            if (hdr_end) {
                header_done = 1;
                *hdr_end = '\0'; // temporary null for parsing headers
                
                // Parse status code
                char *sp = strchr(temp_buffer, ' ');
                if (sp) response_status_code = atoi(sp + 1);
                
                // Parse Content-Length
                char *cl = strcasestr(temp_buffer, "Content-Length:");
                if (cl) content_length = atoll(cl + 15);
                
                // Detect chunked encoding
                char *te = strcasestr(temp_buffer, "Transfer-Encoding:");
                if (te && strcasestr(te, "chunked")) is_chunked = 1;
                
                // Parse Cache-Control directives
                char *cc = strcasestr(temp_buffer, "Cache-Control:");
                if (cc) {
                    if (strcasestr(cc, "no-store") || strcasestr(cc, "private")) {
                        cacheable = 0;
                    }
                    char *ma = strcasestr(cc, "max-age=");
                    if (ma) {
                        long max_age = atol(ma + 8);
                        if (max_age > 0) resp_expiry = time(NULL) + max_age;
                    }
                }
                
                // Parse Expires header (fallback if no max-age)
                if (resp_expiry == 0) {
                    char *exp = strcasestr(temp_buffer, "Expires:");
                    if (exp) {
                        struct tm et = {0};
                        // Parse RFC 1123 date
                        if (strptime(exp + 9, "%a, %d %b %Y %H:%M:%S %Z", &et) != NULL) {
                            resp_expiry = mktime(&et);
                        }
                    }
                }

                // Parse ETag
                char *etag_hdr = strcasestr(temp_buffer, "ETag:");
                if (etag_hdr) {
                    etag_hdr += 5;
                    while (*etag_hdr == ' ') etag_hdr++;
                    char *eol = strstr(etag_hdr, "\r\n");
                    if (!eol) eol = etag_hdr + strlen(etag_hdr);
                    int elen = (eol - etag_hdr < (int)sizeof(resp_etag)-1) ? (eol - etag_hdr) : (int)sizeof(resp_etag)-1;
                    memcpy(resp_etag, etag_hdr, elen);
                    resp_etag[elen] = '\0';
                }
                
                // Parse Last-Modified
                char *lm_hdr = strcasestr(temp_buffer, "Last-Modified:");
                if (lm_hdr) {
                    lm_hdr += 14;
                    while (*lm_hdr == ' ') lm_hdr++;
                    char *eol = strstr(lm_hdr, "\r\n");
                    if (!eol) eol = lm_hdr + strlen(lm_hdr);
                    int llen = (eol - lm_hdr < (int)sizeof(resp_last_modified)-1) ? (eol - lm_hdr) : (int)sizeof(resp_last_modified)-1;
                    memcpy(resp_last_modified, lm_hdr, llen);
                    resp_last_modified[llen] = '\0';
                }
                
                *hdr_end = '\r'; // restore
                
                int hdr_len = (hdr_end - temp_buffer) + 4;
                body_bytes_read = temp_buffer_index - hdr_len;

                if (!cacheable && !is_chunked) {
                    temp_buffer_index = 0; 
                } else if (!cacheable && is_chunked) {
                    if (temp_buffer_index > 5) {
                        memmove(temp_buffer, temp_buffer + temp_buffer_index - 5, 5);
                        temp_buffer_index = 5;
                    }
                }
            }
        } else {
            body_bytes_read += bytes_send;
        }

        if (header_done) {
            if (content_length != -1 && body_bytes_read >= content_length) {
                break;
            }
            if (is_chunked) {
                if (temp_buffer_index >= 5 && memcmp(&temp_buffer[temp_buffer_index-5], "0\r\n\r\n", 5) == 0) {
                    break;
                }
            }
        }
		bzero(buf, MAX_BYTES);
	} 

    if (cacheable && header_done && response_status_code != 304) {
	    temp_buffer[temp_buffer_index]='\0';
	    add_cache_element(temp_buffer, temp_buffer_index, tempReq, resp_expiry,
                         resp_etag[0] ? resp_etag : NULL,
                         resp_last_modified[0] ? resp_last_modified : NULL);
    }
	free(buf);
	free(temp_buffer);
 	close(remoteSocketID);
	return response_status_code;
}


int checkHTTPversion(char *msg)
{
	int version = -1;

	if(strncmp(msg, "HTTP/1.1", 8) == 0)
	{
		version = 1;
	}
	else if(strncmp(msg, "HTTP/1.0", 8) == 0)			
	{
		version = 1;										// Handling this similar to version 1.1
	}
	else
		version = -1;

	return version;
}


void* thread_fn(void* socketNew)
{
	sem_wait(&seamaphore); 
	int p;
	sem_getvalue(&seamaphore,&p);
	printf("semaphore value:%d\n",p);
    int* t= (int*)(socketNew);
	int socket=*t;           // Socket is socket descriptor of the connected Client

	struct sockaddr_in client_addr;
	socklen_t client_len = sizeof(client_addr);
	char client_ip[INET_ADDRSTRLEN] = "Unknown";
	if (getpeername(socket, (struct sockaddr*)&client_addr, &client_len) == 0) {
		inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, INET_ADDRSTRLEN);
	}

    pthread_mutex_lock(&metrics_lock);
    metric_total_requests++;
    metric_active_connections++;
    pthread_mutex_unlock(&metrics_lock);

	struct timespec start_time, end_time;
	clock_gettime(CLOCK_MONOTONIC, &start_time);

	int bytes_send_client,len;	  // Bytes Transferred
    int response_status = 200;
    int bytes_transferred = 0;
    const char* cache_status = "MISS";
    char req_method[16] = "UNKNOWN";
    char req_url[4096] = "-";
	
	char *buffer = (char*)calloc(MAX_BYTES,sizeof(char));	// Creating buffer of 4kb for a client

	if (!is_allowed_ip(client_ip)) {
        char *msg = "HTTP/1.1 403 Forbidden\r\nContent-Length: 13\r\n\r\n403 Forbidden";
        bytes_transferred = send(socket, msg, strlen(msg), 0);
        response_status = 403;
        goto cleanup;
    }

    if (!check_rate_limit(client_ip)) {
        char *msg = "HTTP/1.1 429 Too Many Requests\r\nContent-Length: 21\r\n\r\n429 Too Many Requests";
        bytes_transferred = send(socket, msg, strlen(msg), 0);
        response_status = 429;
        goto cleanup;
    }

	
	
	bzero(buffer, MAX_BYTES);								// Making buffer zero
	bytes_send_client = recv(socket, buffer, MAX_BYTES, 0); // Receiving the Request of client by proxy server
	
	while(bytes_send_client > 0)
	{
		len = strlen(buffer);
        //loop until u find "\r\n\r\n" in the buffer
		if(strstr(buffer, "\r\n\r\n") == NULL)
		{	
			bytes_send_client = recv(socket, buffer + len, MAX_BYTES - len, 0);
		}
		else{
			break;
		}
	}

	// printf("--------------------------------------------\n");
	// printf("%s\n",buffer);
	// printf("----------------------%d----------------------\n",strlen(buffer));
	
	char *tempReq = (char*)malloc(strlen(buffer)*sizeof(char)+1);
    //tempReq, buffer both store the http request sent by client
	for (int i = 0; i < strlen(buffer); i++)
	{
		tempReq[i] = buffer[i];
	}
	
	//checking for the request in cache 
	struct cache_element* temp = find(tempReq);
	const char *stale_etag = NULL;
	const char *stale_last_modified = NULL;

	if( temp != NULL){
        //request found in cache, so sending the response to client from proxy's cache
        pthread_mutex_lock(&metrics_lock);
        metric_cache_hits++;
        pthread_mutex_unlock(&metrics_lock);
        cache_status = "HIT";

		int size=temp->len/sizeof(char);
		int pos=0;
		char response[MAX_BYTES];
		while(pos<size){
			bzero(response,MAX_BYTES);
			for(int i=0;i<MAX_BYTES;i++){
				response[i]=temp->data[pos];
				pos++;
			}
			bytes_transferred += send(socket,response,MAX_BYTES,0);
		}
		printf("Data retrived from the Cache\n\n");
		printf("%s\n\n",response);
	}
	else if(bytes_send_client > 0)
	{
		// Cache miss (or expired) — check for stale validators for conditional GET
		pthread_mutex_lock(&lock);
		cache_element *raw = head;
		while (raw) {
			if (!strcmp(raw->url, tempReq)) {
				stale_etag = raw->etag;
				stale_last_modified = raw->last_modified;
				break;
			}
			raw = raw->next;
		}
		pthread_mutex_unlock(&lock);

		len = strlen(buffer); 
		//Parsing the request
		ParsedRequest* request = ParsedRequest_create();
		
        //ParsedRequest_parse returns 0 on success and -1 on failure.On success it stores parsed request in
        // the request
		if (ParsedRequest_parse(request, buffer, len) < 0) 
		{
		   	printf("Parsing failed\n");
            sendErrorMessage(socket, 400);
            response_status = 400;
		}
		else
		{	
			bzero(buffer, MAX_BYTES);
            
            strncpy(req_method, request->method, sizeof(req_method)-1);
            if (request->path) {
                strncpy(req_url, request->path, sizeof(req_url)-1);
            }

            if (request->path && (!strcmp(request->path, "/status") || !strcmp(request->path, "/proxy-status"))) {
                char status_buf[1024];
                pthread_mutex_lock(&metrics_lock);
                double hit_ratio = 0.0;
                unsigned long long total = metric_cache_hits + metric_cache_misses;
                if (total > 0) hit_ratio = (double)metric_cache_hits / (double)total;
                int out_len = snprintf(status_buf, sizeof(status_buf),
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: application/json\r\n"
                    "Connection: close\r\n"
                    "\r\n"
                    "{\n"
                    "  \"requests\": %llu,\n"
                    "  \"connections\": %llu,\n"
                    "  \"cache_hits\": %llu,\n"
                    "  \"cache_misses\": %llu,\n"
                    "  \"cache_hit_ratio\": %.2f,\n"
                    "  \"cache_size\": %d\n"
                    "}\n",
                    metric_total_requests, metric_active_connections,
                    metric_cache_hits, metric_cache_misses,
                    hit_ratio, cache_size);
                pthread_mutex_unlock(&metrics_lock);
                bytes_transferred = send(socket, status_buf, out_len, 0);
                response_status = 200;
            }
			else
			{	
                // Proxy Authentication
                struct ParsedHeader *auth_hdr = ParsedHeader_get(request, "Proxy-Authorization");
                int auth_ok = 0;
                if (auth_hdr && strstr(auth_hdr->value, "Basic dXNlcjpwYXNz") != NULL) {
                    auth_ok = 1;
                }
                
                if (!auth_ok) {
                    char *msg = "HTTP/1.1 407 Proxy Authentication Required\r\nProxy-Authenticate: Basic realm=\"Proxy\"\r\nContent-Length: 33\r\n\r\n407 Proxy Authentication Required";
                    bytes_transferred = send(socket, msg, strlen(msg), 0);
                    response_status = 407;
                } else if (!strcmp(request->method, "CONNECT")) {
                    int server_port = 443;
                    if (request->port != NULL) {
                        server_port = atoi(request->port);
                    }
                    int remoteSocketID = connectRemoteServer(request->host, server_port);
                    if (remoteSocketID < 0) {
                        sendErrorMessage(socket, 500);
                        response_status = 500;
                    } else {
                        char *msg = "HTTP/1.1 200 Connection Established\r\n\r\n";
                        bytes_transferred += send(socket, msg, strlen(msg), 0);
                        tunnel_data(socket, remoteSocketID);
                        close(remoteSocketID);
                        response_status = 200;
                    }
                } else if (!strcmp(request->method,"GET"))
			    {

                
				if( request->host && request->path && (checkHTTPversion(request->version) == 1) )
				{
					bytes_send_client = handle_request(socket, request, tempReq, stale_etag, stale_last_modified);	// Handle GET request
					if(bytes_send_client == -1)
					{	
						sendErrorMessage(socket, 500);
                        response_status = 500;
					}

				}
				else {
					sendErrorMessage(socket, 500);			// 500 Internal Error
                    response_status = 500;
                }

			}
            else
            {
                printf("This code doesn't support any method other than GET\n");
                sendErrorMessage(socket, 501);
                response_status = 501;
            }
            } // Close auth block
		}
        //freeing up the request pointer
		ParsedRequest_destroy(request);

	}

	else if( bytes_send_client < 0)
	{
		perror("Error in receiving from client.\n");
	}
	else if(bytes_send_client == 0)
	{
		printf("Client disconnected!\n");
	}

cleanup:
	shutdown(socket, SHUT_RDWR);
	close(socket);
	free(buffer);
	sem_post(&seamaphore);	
	
	sem_getvalue(&seamaphore,&p);
	printf("Semaphore post value:%d\n",p);
	free(tempReq);

	clock_gettime(CLOCK_MONOTONIC, &end_time);
	int latency = (end_time.tv_sec - start_time.tv_sec) * 1000 + (end_time.tv_nsec - start_time.tv_nsec) / 1000000;
	
    pthread_mutex_lock(&metrics_lock);
    metric_active_connections--;
    if (strcmp(cache_status, "MISS") == 0) {
        metric_cache_misses++;
    }
    pthread_mutex_unlock(&metrics_lock);

    log_access(client_ip, req_method, req_url, response_status, bytes_transferred, latency, cache_status);

	return NULL;
}


int main(int argc, char * argv[]) {

	int client_socketId, client_len; // client_socketId == to store the client socket id
	struct sockaddr_in server_addr, client_addr; // Address of client and server to be assigned

    sem_init(&seamaphore,0,MAX_CLIENTS); // Initializing seamaphore and lock
    pthread_mutex_init(&lock,NULL); // Initializing lock for cache
    

	if(argc == 2)        //checking whether two arguments are received or not
	{
		port_number = atoi(argv[1]);
	}
	else
	{
		printf("Too few arguments\n");
		exit(1);
	}

	printf("Setting Proxy Server Port : %d\n",port_number);

    //creating the proxy socket
	proxy_socketId = socket(AF_INET, SOCK_STREAM, 0);

	if( proxy_socketId < 0)
	{
		perror("Failed to create socket.\n");
		exit(1);
	}

	int reuse =1;
	if (setsockopt(proxy_socketId, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse)) < 0) 
        perror("setsockopt(SO_REUSEADDR) failed\n");

	bzero((char*)&server_addr, sizeof(server_addr));  
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(port_number); // Assigning port to the Proxy
	server_addr.sin_addr.s_addr = INADDR_ANY; // Any available adress assigned

    // Binding the socket
	if( bind(proxy_socketId, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0 )
	{
		perror("Port is not free\n");
		exit(1);
	}
	printf("Binding on port: %d\n",port_number);

    // Proxy socket listening to the requests
	int listen_status = listen(proxy_socketId, MAX_CLIENTS);

	if(listen_status < 0 )
	{
		perror("Error while Listening !\n");
		exit(1);
	}

	int i = 0; // Iterator for thread_id (tid) and Accepted Client_Socket for each thread
	int Connected_socketId[MAX_CLIENTS];   // This array stores socket descriptors of connected clients

    // Infinite Loop for accepting connections
	while(1)
	{
		
		bzero((char*)&client_addr, sizeof(client_addr));			// Clears struct client_addr
		client_len = sizeof(client_addr); 

        // Accepting the connections
		client_socketId = accept(proxy_socketId, (struct sockaddr*)&client_addr,(socklen_t*)&client_len);	// Accepts connection
		if(client_socketId < 0)
		{
			fprintf(stderr, "Error in Accepting connection !\n");
			exit(1);
		}
		else{
			Connected_socketId[i] = client_socketId; // Storing accepted client into array
		}

		// Getting IP address and port number of client
		struct sockaddr_in* client_pt = (struct sockaddr_in*)&client_addr;
		struct in_addr ip_addr = client_pt->sin_addr;
		char str[INET_ADDRSTRLEN];										// INET_ADDRSTRLEN: Default ip address size
		inet_ntop( AF_INET, &ip_addr, str, INET_ADDRSTRLEN );
		printf("Client is connected with port number: %d and ip address: %s \n",ntohs(client_addr.sin_port), str);
		//printf("Socket values of index %d in main function is %d\n",i, client_socketId);
		pthread_create(&tid[i],NULL,thread_fn, (void*)&Connected_socketId[i]); // Creating a thread for each client accepted
		i++; 
	}
	close(proxy_socketId);									// Close socket
 	return 0;
}

cache_element* find(char* url){

// Checks for url in the cache if found returns pointer to the respective cache element or else returns NULL
    cache_element* site=NULL;
    int temp_lock_val = pthread_mutex_lock(&lock);
    printf("Remove Cache Lock Acquired %d\n",temp_lock_val); 
    if(head!=NULL){
        site = head;
        while (site!=NULL)
        {
            if(!strcmp(site->url,url)){
				printf("LRU Time Track Before : %ld", site->lru_time_track);
                printf("\nurl found\n");
				// Check expiry before returning
                if (site->expiry_time > 0 && time(NULL) >= site->expiry_time) {
                    printf("\nCache entry expired\n");
                    site = NULL;  // treat as miss; caller will send conditional GET
                    break;
                }
				// Updating the time_track
				site->lru_time_track = time(NULL);
				printf("LRU Time Track After : %ld", site->lru_time_track);
				break;
            }
            site=site->next;
        }       
    }
	else {
    printf("\nurl not found\n");
	}
    temp_lock_val = pthread_mutex_unlock(&lock);
    printf("Remove Cache Lock Unlocked %d\n",temp_lock_val); 
    return site;
}

void remove_cache_element(){
    // If cache is not empty searches for the node which has the least lru_time_track and deletes it
    cache_element * p ;  	// Cache_element Pointer (Prev. Pointer)
	cache_element * q ;		// Cache_element Pointer (Next Pointer)
	cache_element * temp;	// Cache element to remove
    int temp_lock_val = pthread_mutex_lock(&lock);
	printf("Remove Cache Lock Acquired %d\n",temp_lock_val); 
	if( head != NULL) { // Cache != empty
		for (q = head, p = head, temp =head ; q -> next != NULL; 
			q = q -> next) { // Iterate through entire cache and search for oldest time track
			if(( (q -> next) -> lru_time_track) < (temp -> lru_time_track)) {
				temp = q -> next;
				p = q;
			}
		}
		if(temp == head) { 
			head = head -> next; /*Handle the base case*/
		} else {
			p->next = temp->next;	
		}
		cache_size = cache_size - (temp -> len) - sizeof(cache_element) - 
		strlen(temp -> url) - 1;     //updating the cache size
		free(temp->data);     		
		free(temp->url); // Free the removed element
		if (temp->etag) free(temp->etag);
		if (temp->last_modified) free(temp->last_modified);
		free(temp);
	} 
    temp_lock_val = pthread_mutex_unlock(&lock);
	printf("Remove Cache Lock Unlocked %d\n",temp_lock_val); 
}

int add_cache_element(char* data, int size, char* url, time_t expiry, const char* etag, const char* last_modified){
    // Adds element to the cache
    int temp_lock_val = pthread_mutex_lock(&lock);
	printf("Add Cache Lock Acquired %d\n", temp_lock_val);
    int element_size=size+1+strlen(url)+sizeof(cache_element); // Size of the new element which will be added to the cache
    if(element_size>MAX_ELEMENT_SIZE){
        // If element size is greater than MAX_ELEMENT_SIZE we don't add the element to the cache
        temp_lock_val = pthread_mutex_unlock(&lock);
		printf("Add Cache Lock Unlocked %d\n", temp_lock_val);
        return 0;
    }
    else
    {   while(cache_size+element_size>MAX_SIZE){
            // We keep removing elements from cache until we get enough space to add the element
            remove_cache_element();
        }
        cache_element* element = (cache_element*) malloc(sizeof(cache_element)); // Allocating memory for the new cache element
        element->data = (char*)malloc(size+1); // Allocating memory for the response to be stored in the cache element
		memcpy(element->data, data, size);
		element->data[size] = '\0';
        element->url = (char*)malloc(1+(strlen(url)*sizeof(char))); // Allocating memory for the request to be stored in the cache element (as a key)
		strcpy(element->url, url);
		element->lru_time_track = time(NULL);    // Updating the time_track
        element->next = head; 
        element->len = size;
        element->expiry_time = expiry;
        element->etag = etag ? strdup(etag) : NULL;
        element->last_modified = last_modified ? strdup(last_modified) : NULL;
        head = element;
        cache_size += element_size;
        temp_lock_val = pthread_mutex_unlock(&lock);
		printf("Add Cache Lock Unlocked %d\n", temp_lock_val);
        return 1;
    }
    return 0;
}