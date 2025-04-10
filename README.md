# HTTP Caching Proxy

A multi-threaded HTTP proxy server written in C that relays requests from clients to HTTP servers and caches responses for performance improvements\

## Features

- Handles HTTP `GET` requests from clients.
- Parses and validates HTTP requests.
- Caches server responses locally for future use.
- Automatically skips caching for dynamic content.
- Multi-threaded: handles multiple clients concurrently using POSIX threads.
- Graceful shutdown on `Ctrl + C` (SIGINT).
- Cache expiration using a user-defined timeout value.

## How It Works

1. The proxy listens for HTTP client connections on a specified port.
2. Upon receiving a valid `GET` request, it parses the URL into host, port, and path.
3. If a cached version of the response exists and is valid (not expired), the proxy returns it directly to the client.
4. If not, it forwards the request to the appropriate web server, caches the result if it is static content, and relays the response to the client.

## Usage

### Compilation

```bash
gcc proxy.c -lssl -lcrypto -lpthread -o proxy
