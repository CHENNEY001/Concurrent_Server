/*
 * multi-server.c
 */

#include <stdio.h>      /* for printf() and fprintf() */
#include <sys/socket.h> /* for socket(), bind(), and connect() */
#include <arpa/inet.h>  /* for sockaddr_in and inet_ntoa() */
#include <stdlib.h>     /* for atoi() and exit() */
#include <string.h>     /* for memset() */
#include <unistd.h>     /* for close() */
#include <time.h>       /* for time() */
#include <netdb.h>      /* for gethostbyname() */
#include <signal.h>     /* for signal() */
#include <sys/stat.h>   /* for stat() */
#include <pthread.h>    /* for pthread */
#include <sys/select.h> /* for select() */

#define MAXPENDING 5    /* Maximum outstanding connection requests */

#define DISK_IO_BUF_SIZE 4096
#define MAX_ARG 300
#define N_THREADS 16

static pthread_t thread_pool[N_THREADS];

static void die(const char *message)
{
    perror(message);
    exit(1);
}

/*
 * Create a listening socket bound to the given port.
 */
static int createServerSocket(unsigned short port)
{
    int servSock;
    struct sockaddr_in servAddr;

    /* Create socket for incoming connections */
    if ((servSock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0)
        die("socket() failed");

    /* Construct local address structure */
    memset(&servAddr, 0, sizeof(servAddr));       /* Zero out structure */
    servAddr.sin_family = AF_INET;                /* Internet address family */
    servAddr.sin_addr.s_addr = htonl(INADDR_ANY); /* Any incoming interface */
    servAddr.sin_port = htons(port);              /* Local port */

    /* Bind to the local address */
    if (bind(servSock, (struct sockaddr *)&servAddr, sizeof(servAddr)) < 0)
        die("bind() failed");

    /* Mark the socket so it will listen for incoming connections */
    if (listen(servSock, MAXPENDING) < 0)
        die("listen() failed");

    return servSock;
}

/*
 * A wrapper around send() that does error checking and logging.
 * Returns -1 on failure.
 *
 * This function assumes that buf is a null-terminated string, so
 * don't use this function to send binary data.
 */
ssize_t Send(int sock, const char *buf)
{
    size_t len = strlen(buf);
    ssize_t res = send(sock, buf, len, 0);
    if (res != len) {
        perror("send() failed");
        return -1;
    }
    else
        return res;
}

/*
 * HTTP/1.0 status codes and the corresponding reason phrases.
 */

static struct {
    int status;
    char *reason;
} HTTP_StatusCodes[] = {
    { 200, "OK" },
    { 201, "Created" },
    { 202, "Accepted" },
    { 204, "No Content" },
    { 301, "Moved Permanently" },
    { 302, "Moved Temporarily" },
    { 304, "Not Modified" },
    { 400, "Bad Request" },
    { 401, "Unauthorized" },
    { 403, "Forbidden" },
    { 404, "Not Found" },
    { 500, "Internal Server Error" },
    { 501, "Not Implemented" },
    { 502, "Bad Gateway" },
    { 503, "Service Unavailable" },
    { 0, NULL } // marks the end of the list
};

struct message {
    int sock; // Payload, in our case a new client connection
    struct message *next; // Next message on the list
};

/*
 * This structure implements a blocking queue.
 * If a thread attempts to pop an item from an empty queue
 * it is blocked until another thread appends a new item.
 */

struct queue {
    pthread_mutex_t mutex; // mutex used to protect the queue
    pthread_cond_t cond;   // condition variable for threads to sleep on
    struct message *first; // first message in the queue
    struct message *last;  // last message in the queue
    unsigned int length;   // number of elements on the queue
};

struct queue g_queue;

// initializes the members of struct queue
void queue_init(struct queue *q) {
    q->first = NULL;
    q->last = NULL;
    q->length = 0;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
}

// deallocate and destroy everything in the queue
void queue_destroy(struct queue *q) {
    pthread_mutex_lock(&q->mutex);

    struct message *mess = malloc(sizeof(struct message));
    mess = q->first;
    while (mess != NULL) {
        struct message *tmp = mess->next;
        free(mess);
        mess = tmp;
    }

    pthread_mutex_unlock(&q->mutex);
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond);
}

// put a message into the queue and wake up workers if necessary
void queue_put(struct queue *q, int sock){
    struct message *mess = malloc(sizeof(struct message));
    mess->sock = sock; // assign client socket to mess

    pthread_mutex_lock(&q->mutex); // lock mutex;

    if (q->last == NULL) {
        q->first = q->last = mess;
    } else {
        q->last->next = mess;
        q->last = mess;
    }
    q->length++;

    pthread_cond_signal(&q->cond); // wake blocked threads with cond
    pthread_mutex_unlock(&q->mutex); //unlock mutex

//    fprintf(stderr, "put socket into the queue: %d by function queue_put\n", sock);
}

// take a socket descriptor from the queue; block if necessary
int queue_get(struct queue *q) {
    pthread_mutex_lock(&q->mutex);
    while (q->length == 0) { // block if necessary
        pthread_cond_wait(&q->cond, &q->mutex);
    }

    struct message *mess = malloc(sizeof(struct message));
    mess = q->first;
    int sock = mess->sock;
    q->first = mess->next;
    q->length--;

    if (q->length == 0) {
        q->last = NULL;
    }

    free(mess);
    pthread_mutex_unlock(&q->mutex);

//    fprintf(stderr, "get socket from the queue: %d by function queue_get\n", sock);

    return sock;
}

struct net_info{
    int servSock;
    char webroot[1000];
    struct sockaddr_in clntAddr;
};

static inline const char *getReasonPhrase(int statusCode)
{
    int i = 0;
    while (HTTP_StatusCodes[i].status > 0) {
        if (HTTP_StatusCodes[i].status == statusCode)
            return HTTP_StatusCodes[i].reason;
        i++;
    }
    return "Unknown Status Code";
}


/*
 * Send HTTP status line followed by a blank line.
 */
static void sendStatusLine(int clntSock, int statusCode)
{
    char buf[1000];
    const char *reasonPhrase = getReasonPhrase(statusCode);

    // print the status line into the buffer
    sprintf(buf, "HTTP/1.0 %d ", statusCode);
    strcat(buf, reasonPhrase);
    strcat(buf, "\r\n");

    // We don't send any HTTP header in this simple server.
    // We need to send a blank line to signal the end of headers.
    strcat(buf, "\r\n");

    // For non-200 status, format the status line as an HTML content
    // so that browers can display it.
    if (statusCode != 200) {
        char body[1000];
        sprintf(body,
                "<html><body>\n"
                "<h1>%d %s</h1>\n"
                "</body></html>\n",
                statusCode, reasonPhrase);
        strcat(buf, body);
    }

    // send the buffer to the browser
    Send(clntSock, buf);
}

/*
 * Handle static file requests.
 * Returns the HTTP status code that was sent to the browser.
 */
static int handleFileRequest(
        const char *webRoot, const char *requestURI, int clntSock)
{
    int statusCode;
    FILE *fp = NULL;

    // Compose the file path from webRoot and requestURI.
    // If requestURI ends with '/', append "index.html".

    char *file = (char *)malloc(strlen(webRoot) + strlen(requestURI) + 100);
    if (file == NULL)
        die("malloc failed");
    strncpy(file, webRoot, 100);
    strcat(file, requestURI);
    if (file[strlen(file)-1] == '/') {
        strcat(file, "index.html");
    }

    // See if the requested file is a directory.
    // Our server does not support directory listing.

    struct stat st;
    if (stat(file, &st) == 0 && S_ISDIR(st.st_mode)) {
        statusCode = 403; // "Forbidden"
        sendStatusLine(clntSock, statusCode);
        goto func_end;
    }

    // If unable to open the file, send "404 Not Found".

    fp = fopen(file, "rb");
    if (fp == NULL) {
        statusCode = 404; // "Not Found"
        sendStatusLine(clntSock, statusCode);
        goto func_end;
    }

    // Otherwise, send "200 OK" followed by the file content.

    statusCode = 200; // "OK"
    sendStatusLine(clntSock, statusCode);

    // send the file
    size_t n;
    char buf[DISK_IO_BUF_SIZE];
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (send(clntSock, buf, n, 0) != n) {
            // send() failed.
            // We log the failure, break out of the loop,
            // and let the server continue on with the next request.
            perror("\nsend() failed");
            break;
        }
    }
    // fread() returns 0 both on EOF and on error.
    // Let's check if there was an error.
    if (ferror(fp))
        perror("fread failed");

func_end:

    // clean up
    free(file);
    if (fp)
        fclose(fp);

    return statusCode;
}

void network_service(int clntSock, const char *webRoot, struct sockaddr_in clntAddr){

    char line[1000];
    char requestLine[1000];
    int statusCode;

    FILE *clntFp = fdopen(clntSock, "r");
    if (clntFp == NULL)
        die("fdopen failed");

    /*
     * Let's parse the request line.
     */

    char *method      = "";
    char *requestURI  = "";
    char *httpVersion = "";

    if (fgets(requestLine, sizeof(requestLine), clntFp) == NULL) {
        // socket closed - there isn't much we can do
        statusCode = 400; // "Bad Request"
        goto loop_end;
    }

    char *token_separators = "\t \r\n"; // tab, space, new line
    char *lasts;
    method = strtok_r(requestLine, token_separators, &lasts);
    requestURI = strtok_r(NULL, token_separators, &lasts);
    httpVersion = strtok_r(NULL, token_separators, &lasts);
    char *extraThingsOnRequestLine = strtok_r(NULL, token_separators, &lasts);

    // check if we have 3 (and only 3) things in the request line
    if (!method || !requestURI || !httpVersion ||
        extraThingsOnRequestLine) {
        statusCode = 501; // "Not Implemented"
        sendStatusLine(clntSock, statusCode);
        goto loop_end;
    }

    // we only support GET method
    if (strcmp(method, "GET") != 0) {
        statusCode = 501; // "Not Implemented"
        sendStatusLine(clntSock, statusCode);
        goto loop_end;
    }

    // we only support HTTP/1.0 and HTTP/1.1
    if (strcmp(httpVersion, "HTTP/1.0") != 0 &&
        strcmp(httpVersion, "HTTP/1.1") != 0) {
        statusCode = 501; // "Not Implemented"
        sendStatusLine(clntSock, statusCode);
        goto loop_end;
    }

    // requestURI must begin with "/"
    if (!requestURI || *requestURI != '/') {
        statusCode = 400; // "Bad Request"
        sendStatusLine(clntSock, statusCode);
        goto loop_end;
    }

    // make sure that the requestURI does not contain "/../" and
    // does not end with "/..", which would be a big security hole!
    int len = strlen(requestURI);
    if (len >= 3) {
        char *tail = requestURI + (len - 3);
        if (strcmp(tail, "/..") == 0 ||
            strstr(requestURI, "/../") != NULL)
        {
            statusCode = 400; // "Bad Request"
            sendStatusLine(clntSock, statusCode);
            goto loop_end;
        }
    }

    /*
     * Now let's skip all headers.
     */

    while (1) {
        if (fgets(line, sizeof(line), clntFp) == NULL) {
            // socket closed prematurely - there isn't much we can do
            statusCode = 400; // "Bad Request"
            goto loop_end;
        }
        if (strcmp("\r\n", line) == 0 || strcmp("\n", line) == 0) {
            // This marks the end of headers.
            // Break out of the while loop.
            break;
        }
    }

    /*
     * At this point, we have a well-formed HTTP GET request.
     * Let's handle it.
     */

    statusCode = handleFileRequest(webRoot, requestURI, clntSock);

    loop_end:

    /*
     * Done with client request.
     * Log it, close the client socket, and go back to accepting
     * connection.
     */

    fprintf(stderr, "%s (%d) \"%s %s %s\" %d %s\n",
            inet_ntoa(clntAddr.sin_addr),
            getpid(),
            method,
            requestURI,
            httpVersion,
            statusCode,
            getReasonPhrase(statusCode));

    // close the client socket
    fclose(clntFp);
}

void *worker_thread(void *argv) {

    struct net_info* para = (struct net_info *)(argv);
//    fprintf(stderr, "This is for debug: successfully get argv\n");

    char webroot[1000];
    strncpy(webroot, para->webroot, sizeof(webroot)-1);

    for (;;) {
        int clntSock = queue_get(&g_queue);
//        fprintf(stderr, "This is for debug: successfully get socket number from queue\n");
        network_service(clntSock, webroot, para->clntAddr);
    }

    return 0;
}

int main(int argc, char *argv[])
{
    // Ignore SIGPIPE so that we don't terminate when we call
    // send() on a disconnected socket.
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
        die("signal() failed");

    int servSocks[32];
    memset(servSocks, -1, sizeof(servSocks));

// Create server sockets for all ports we listen on
    for (int i = 1; i < argc - 1; i++) {
        if (i >= (sizeof(servSocks) / sizeof(servSocks[0])))
            die("Too many listening sockets");
        servSocks[i - 1] = createServerSocket(atoi(argv[i]));
    }

    struct timeval check_time; // Query time
    check_time.tv_sec = 3 * 60; // turn off in 3 minutes, if there is no activity.
    check_time.tv_usec = 0;

    fd_set read_fds;

    struct net_info info;
    queue_init(&g_queue);

    strncpy(info.webroot, argv[argc - 1], sizeof(info.webroot));
    unsigned int clntLen = sizeof(info.clntAddr);

    for (int i = 0; i < N_THREADS; i++) {
        pthread_create(&thread_pool[i], NULL, worker_thread, (void *)&info);
    }

    for (;;) {
        FD_ZERO(&read_fds);
        for (int i = 0; i < 32; i++) {
            if (servSocks[i] == -1)
                break;
            FD_SET(servSocks[i], &read_fds);
        }

        if (select(32 + 1, &read_fds, NULL, NULL, &check_time) == -1)
            die("Error in select()");

        for (int j = 0; j < 32; ++j) {
            if (!FD_ISSET(servSocks[j], &read_fds)) // servSocks[j] need to be ready
                continue;

            int client_sock = accept(servSocks[j], (struct sockaddr *) &(info.clntAddr), &clntLen);
            if (client_sock < 0)
                die("accept() failed");
//          fprintf(stderr, "This is for debug: successfully accepted\n");
            queue_put(&g_queue, client_sock);
//          fprintf(stderr, "This is for debug: successfully add to the queue\n");
        }
    }

    return 0;
}