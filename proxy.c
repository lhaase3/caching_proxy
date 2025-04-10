/* to compile I run gcc proxy.c -lssl -lcrypto -lpthread -o proxy */

#include <stdio.h>
#include <string.h>	
#include <stdlib.h>	
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h> 
#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <openssl/md5.h>
#include <netdb.h>

#define BUFSIZE 8192
#define CACHE_DIR "./cache"

int proxy_sock;
int timeout;
int server_socket;

/* handles ctrl+c of proxy*/
void sigint_handler(int sig) {
    printf("\nShutting down proxy...\n");
    close(proxy_sock);
    exit(0);
}

/* hash urls */
void md5(const char *url, char *output) {
    /* https://stackoverflow.com/questions/7627723/how-to-create-a-md5-hash-of-a-string-in-c - used this to figure out md5 */
    unsigned char digest[MD5_DIGEST_LENGTH];
    MD5((unsigned char *)url, strlen(url), digest);
    for (int i = 0; i < MD5_DIGEST_LENGTH; i++) {
        sprintf(&output[i * 2], "%02x", digest[i]);
    }
}

/* check if a cached file exists and if it is valid */
int check_cache(const char *hash, char *filename) {
    snprintf(filename, 512, "%s/%s", CACHE_DIR, hash);
    /* https://codeforwin.org/c-programming/c-program-find-file-properties-using-stat-function?utm - used this to figure out st_mtime */
    struct stat st;
    if (stat(filename, &st) != 0) return 0;
    time_t curr_time = time(NULL);
    int time_passed = curr_time - st.st_mtime; 
    return time_passed < timeout;
}

/* if a valid cache is found read it and send it to client */
int send_cache(const char *filename, int client_sock) {
    char buf[BUFSIZE];
    ssize_t bytes_read;
    FILE *file = fopen(filename, "rb");
    if (!file) return -1;
    while ((bytes_read = fread(buf, 1, sizeof(buf), file)) > 0) {
        send(client_sock, buf, bytes_read, 0);
    }
    fclose(file);
}

/* saves the cached file */
void save_cache(const char *filename, const char *data, size_t length) {
    FILE *file = fopen(filename, "wb");
    if (!file) return;
    fwrite(data, 1, length, file);
    fclose(file);
}

/* handles when a status code error occurs */
void error_handler(int client_sock, const char *status_code) {
	char response[1024];
	snprintf(response, sizeof(response), "HTTP/1.1 %s\r\nContent-Type: N/A\r\nContent-Length: 0\r\n\r\n", status_code);
	send(client_sock, response, strlen(response), 0);
}

/* parses the request to get the needed info*/
int request_handler(const char *request, char *host, int* port, char* path, int client_sock) {
    char method[16], url[1024], version[16];
    *port = 80;

    if (sscanf(request, "%s %s %s", method, url, version) != 3 || strcmp(method, "GET") != 0) {
        error_handler(client_sock, "400 Bad Request");
        return -1;
    }

    const char *host_start;
    if (strstr(url, "http://") != NULL) {
        host_start = strstr(url, "http://") + 7;
    } else {
        error_handler(client_sock, "400 Bad Request");
        return -1;
    }

    const char *path_start = strchr(host_start, '/');
    if (path_start) { // if a "/" is found everything past it is the path and everything before is the host
        strcpy(path, path_start);
        int host_len = path_start - host_start;
        strncpy(host, host_start, host_len);
        host[host_len] = '\0';
    } else { // no path is found so set it to "/"
        strcpy(host, host_start);
        strcpy(path, "/");
    }

    char *port_num = strchr(host, ':');
    if (port_num) { // if a ":" is found everything after is the port
        *port_num = '\0';
        *port = atoi(port_num + 1);
    } else { // no port is provided so 80 will be default
        *port = 80;
    }

    return 0;
}

/* establishes a TCP connection to the specified host and port */
int connect_to_server(const char* host, int port, int client_sock) {
    struct hostent* server = gethostbyname(host); // performs DNS to resolve host into IP addr
    if (!server) return -1;
    /* https://beej.us/guide/bgnet/html/#connectman - used this for memset and memcpy, making a new struct sockaddr_in was giving me issues and I couldn't figure it out*/
    struct sockaddr_in serv_addr;
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length); // copies IP addr from the server->h_addr from gethostname

    if (connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) { // attempts to connect the socket to the remote server
        error_handler(client_sock, "404 Not Found");
        return -1;
    }

    return sockfd;
}

/* fetches from server and optionally caches */
int fetch_and_cache(const char* host, int port, const char* path, const char* full_url, int client_sock) {
    char response[BUFSIZE];
    char server_request[2048];
    int total_len = 0;
    ssize_t request_size;

    int server_sock = connect_to_server(host, port, client_sock); // connect to server based on parsed HTTP request
    if (server_sock < 0) return -1;

    printf("Connected to remote server: %s:%d. Forwarding request...\n", host, port);
    snprintf(server_request, sizeof(server_request), "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n", path, host); // constructing server request
    printf("Final request to server:\n%s\n", server_request);
    send(server_sock, server_request, strlen(server_request), 0); 

    char* full_response = malloc(1);
    full_response[0] = '\0';

    while ((request_size = recv(server_sock, response, BUFSIZE, 0)) > 0) {  // continuoussly read data from server
        send(client_sock, response, request_size, 0);
        full_response = realloc(full_response, total_len + request_size);
        memcpy(full_response + total_len, response, request_size);
        total_len += request_size;
    }

    if (!strchr(path, '?')) { // caching gile if not dynamic
        char hash[33], filename[512];
        md5(full_url, hash);
        snprintf(filename, sizeof(filename), "%s/%s", CACHE_DIR, hash);
        save_cache(filename, full_response, total_len);
        printf("Response saved to cache.\n");
    }

    free(full_response);
    close(server_sock);
    return 0;
}

/* handles each client in its own thread */
void connection_handler(int client_sock) {
    char buf[BUFSIZE], response[BUFSIZE];
    char host[256], path[1024], full_url[2048];
    int port;

    printf("handling new client request...\n");
    ssize_t request_size = recv(client_sock, buf, sizeof(buf) - 1, 0);
    printf("Received request:\n%s\n", buf);
    if (request_size <= 0) {
        // printf("Failed to read request\n");
        perror("recv failed");
        printf("request size: %ld\n", request_size);
        close(client_sock);
        return;
    }
    buf[request_size] = '\0';

    if (request_handler(buf, host, &port, path, client_sock) != 0) { // Bad Request
        close(client_sock);
        return;
    }

    snprintf(full_url, sizeof(full_url), "http://%s:%d%s", host, port, path); // assemble the full url based on the request handler
    printf("Requested URL: %s\n", full_url);

    if (!strchr(path, '?')) { // Check if we should skip cache for dynamic pages
        char hash[33], filename[512];
        md5(full_url, hash);

        if (check_cache(hash, filename)) {
            printf("Cache hit, serving file: %s\n", filename);
            send_cache(filename, client_sock);
            close(client_sock);
            return;
        } else {
            printf("Cache miss, will fetch and save cache...\n");
        }
    } else {
        printf("Skipping cache due to dynamic content\n");
    }

    fetch_and_cache(host, port, path, full_url, client_sock);
    close(client_sock);
}


/* sets up each client for their own thread */
void *client_handler(void *arg) {
	int client_sock = *((int *)arg);
	free(arg);
	connection_handler(client_sock);
	return NULL;
}

int main(int argc, char* argv[]) {
    if (argc != 3) { // expecting ./proxy <port> <timeout>
        fprintf(stderr, "Usage: %s <port> <timeout>\n", argv[0]);
        exit(1);
    }

    int port = atoi(argv[1]);
    timeout = atoi(argv[2]);
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    /* https://stackoverflow.com/questions/7430248/creating-a-new-directory-in-c?utm*/
    mkdir(CACHE_DIR, 0700); // 0700 allows full read, write, and execute permissions
    signal(SIGINT, sigint_handler);

    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr))) {
        perror("Bind failed");
    }
    listen(server_socket, 10);

    while (1) {
        printf("Waiting for a client connection...\n");
        int* client_sock = malloc(sizeof(int));
        *client_sock = accept(server_socket, (struct sockaddr*)&client_addr, &client_len);
        printf("Client connected.\n");

        pthread_t thread;
        pthread_create(&thread, NULL, client_handler, client_sock);
        pthread_detach(thread);
    }

    return 0;
}