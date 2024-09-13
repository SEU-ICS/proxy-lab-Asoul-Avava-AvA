#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define MAX_CACHE_COUNT 10
#define URL_LENGTH 2000
/* You won't lose style points for including this long line in your code */
static const char* user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";

// Reader-writer lock
struct _rwlock {
	sem_t count_lock;
	sem_t writelock;
	int reader_count;
};

// We use cLock to approximate LRU
struct Cache {
	int used;
	char key[MAXLINE];	// We save URL as key here
	char value[MAX_OBJECT_SIZE];
};
struct Cache cache[MAX_CACHE_COUNT];

struct _Url {
	char host[URL_LENGTH];
	char port[URL_LENGTH];
	char path[URL_LENGTH];
};

struct _rwlock* rwlock;

int cLock;      //index of cLock

void doit(int fd);
void set_url(char* dest, const char* src, size_t length);
void parse_url(char* url, struct _Url* u);
void build_http(rio_t* rio, struct _Url* u, char* new_http);
void* thread(void* connfd);
int read_cache(int fd, char* key);
void write_cache(char* buf, char* key);

//mostly copied from tiny
int main(int argc, char** argv) {

	int listenfd, connfd;
	char hostname[URL_LENGTH], port[URL_LENGTH];
	socklen_t clientlen;
	struct sockaddr_storage clientaddr;
	rwlock = Malloc(sizeof(struct _rwlock));
	pthread_t p;

	rwlock->reader_count = 0;
	sem_init(&rwlock->count_lock, 0, 1);
	sem_init(&rwlock->writelock, 0, 1);

	if (argc != 2) {
		fprintf(stderr, "usage: %s <port>\n", argv[0]);
		exit(0);
	}
	listenfd = Open_listenfd(argv[1]);
	while (1) {
		clientlen = sizeof(clientaddr);
		connfd = Accept(listenfd, (SA*)&clientaddr, &clientlen);
		Getnameinfo((SA*)&clientaddr, clientlen, hostname, URL_LENGTH, port, URL_LENGTH, 0);
		printf("Accepted connection from (%s %s)\n", hostname, port);
		
		Pthread_create(&p, NULL, thread, (void*) & connfd);
	}
	return 0;
}


void* thread(void* connfd) {
	int fd =  *(int*)connfd;
	//What if connections come simultaneously so that connfd change too fast? 
	//fd is saved before thread being detached, so we can assign a new connfd after detach.
	Pthread_detach(pthread_self());
	doit(fd);
	close(fd);
	return NULL;
}

void doit(int fd) {
	char buf[MAXLINE], method[MAXLINE], url[MAXLINE], version[MAXLINE], errbuf[MAXLINE];
	char new_http[MAXLINE], temp_url[MAXLINE];
	struct _Url u;
	rio_t rio, server_rio;
	Rio_readinitb(&rio, fd);
	if (!Rio_readlineb(&rio, buf, MAXLINE))
		return;

	sscanf(buf, "%s %s %s", method, url, version);
	strcpy(temp_url, url);

	// Accept GET method only
	if (strcmp(method, "GET") != 0) {
		sprintf(errbuf, "The proxy server can not handle this method: %s\n", method);
		Rio_writen(fd, errbuf, strlen(errbuf));
		return;
	}

	if (read_cache(fd, temp_url) != 0)
		return; // Cache hit, great!

	parse_url(url, &u);
	build_http(&rio, &u, new_http);

	int server_fd = Open_clientfd(u.host, u.port);
	size_t n;

	Rio_readinitb(&server_rio, server_fd);
	Rio_writen(server_fd, new_http, strlen(new_http));

	char cache[MAX_OBJECT_SIZE];
	int sum = 0;
	while ((n = Rio_readlineb(&server_rio, buf, MAXLINE)) != 0) {
		Rio_writen(fd, buf, n);
		sum += n;
		strcat(cache, buf);
	}
	printf("proxy send %d bytes to client\n", sum);
	if (sum < MAX_OBJECT_SIZE)
		write_cache(cache, temp_url);
	close(server_fd);
	return;
}

void write_cache(char* buf, char* key) {
	sem_wait(&rwlock->writelock);
	while (cache[cLock].used != 0) {
		cache[cLock].used = 0;
		cLock = (cLock + 1) % MAX_CACHE_COUNT;
	}
	cache[cLock].used = 1;
	strcpy(cache[cLock].key, key);
	strcpy(cache[cLock].value, buf);
	sem_post(&rwlock->writelock);
	return;
}

int read_cache(int fd, char* url) {
	sem_wait(&rwlock->count_lock);
	if (rwlock->reader_count == 0)
		sem_wait(&rwlock->writelock);
	rwlock->reader_count++;
	sem_post(&rwlock->count_lock);
	int hit = 0;

	for (int i = 0; i < MAX_CACHE_COUNT; i++) {
		//printf ("Yes! %d\n",cache[i].usecnt);
		if (strcmp(url, cache[i].key) == 0) {
			Rio_writen(fd, cache[i].value, strlen(cache[i].value));
			printf("proxy send %ld bytes to client\n", strlen(cache[i].value));
			cache[i].used = 1;
			hit = 1;
			break;
		}
	}

	sem_wait(&rwlock->count_lock);
	rwlock->reader_count--;
	if (rwlock->reader_count == 0)
		sem_post(&rwlock->writelock);
	sem_post(&rwlock->count_lock);
	return hit;
}

// Helper function to set the host, path and port
void set_url(char* dest, const char* src, size_t length) {
	if (length >= URL_LENGTH) {
		length = URL_LENGTH - 1;
	}
	strncpy(dest, src, length);
	dest[length] = '\0';
}

void parse_url(char* url, struct _Url* u) {
	memset(u->host, 0, URL_LENGTH);
	memset(u->port, 0, URL_LENGTH);
	memset(u->path, 0, URL_LENGTH);

	// Find the start of the host part
	const char* hostpose = strstr(url, "//");
	if (hostpose == NULL) {
		hostpose = url; // No '//', start parsing from the beginning
	}
	else {
		hostpose += 2; // Skip '//'
	}

	// Find the port delimiter ':' or the path delimiter '/'
	const char* portpose = strchr(hostpose, ':');
	const char* pathpose = strchr(hostpose, '/');

	// Extract the host
	if (portpose && (!pathpose || portpose < pathpose)) {
		// Port is present and before path
		size_t host_len = portpose - hostpose;
		set_url(u->host, hostpose, host_len);

		// Extract port
		size_t port_len = pathpose ? pathpose - (portpose + 1) : strlen(portpose + 1);
		set_url(u->port, portpose + 1, port_len);

		// Extract path
		if (pathpose) {
			size_t path_len = strlen(pathpose);
			set_url(u->path, pathpose, path_len);
		}
		else {
			strcpy(u->path, "/"); // Default path if none is provided
		}
	}
	else {
		// No port is present
		if (pathpose) {
			size_t host_len = pathpose - hostpose;
			set_url(u->host, hostpose, host_len);

			// Extract path
			size_t path_len = strlen(pathpose);
			set_url(u->path, pathpose, path_len);
		}
		else {
			strcpy(u->host, hostpose); // Only host, no path
			strcpy(u->path, "/"); // Default path
		}

		// Default port
		strcpy(u->port, "80");
	}
}

void build_http(rio_t* rio, struct _Url* u, char* new_http) {
	static const char* Connection_hdr = "Connection: close\r\n";
	static const char* Proxy_connection_hdr = "Proxy-Connection: close\r\n";
	char buf[MAXLINE];
	char Reqline[MAXLINE];
	char Host_hdr[MAXLINE] = { 0 }; // Initialize to ensure no garbage values
	char extra_hdr[MAXLINE] = { 0 };

	// Prepare request line
	snprintf(Reqline, sizeof(Reqline), "GET %s HTTP/1.0\r\n", u->path);

	// Read headers and process
	while (Rio_readlineb(rio, buf, sizeof(buf)) > 0) {
		if (strcmp(buf, "\r\n") == 0) {
			strcat(extra_hdr, "\r\n");
			break;
		}
		else if (strncasecmp(buf, "Host:", 5) == 0) {
			strncpy(Host_hdr, buf, sizeof(Host_hdr) - 1);
		}
		else if (strncasecmp(buf, "Connection:", 11) == 0 ||
			strncasecmp(buf, "Proxy-Connection:", 17) == 0 ||
			strncasecmp(buf, "User-Agent:", 11) == 0) {
			strncat(extra_hdr, buf, sizeof(extra_hdr) - strlen(extra_hdr) - 1);
		}
	}

	// If no Host header found, set it
	if (strlen(Host_hdr) == 0) {
		snprintf(Host_hdr, sizeof(Host_hdr), "Host: %s\r\n", u->host);
	}

	// Construct the new HTTP data
	snprintf(new_http, MAXLINE, "%s%s%s%s%s", Reqline, Host_hdr, Connection_hdr, Proxy_connection_hdr, user_agent_hdr);

	// Append any additional headers and data
	strncat(new_http, extra_hdr, MAXLINE - strlen(new_http) - 1);
}
