#include "nub.h"
#include "uv.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define PORT 7856

/* Globals */
typedef struct {
  uv_write_t req;
  uv_buf_t buf;
} write_req_t;

typedef struct {
  size_t len;
  char* base;
  ssize_t nread;
  uv_stream_t* handle;
} after_write_t;

/* Forward declarations */
static void tcp4_static_echo_server(int port, nub_loop_t* loop);
static void on_connection(uv_stream_t* server, int status);
static void echo_alloc(uv_handle_t* handle,
                       size_t suggested_size,
                       uv_buf_t* buf);
static void after_read(uv_stream_t* handle, ssize_t nread, const uv_buf_t* buf);
static void thread_after_read(nub_thread_t* thread, void* arg);
static void after_write(uv_write_t* req, int status);
static void on_close(uv_handle_t* peer);
static void check_error(int r, const char* msg);


/* Entry point */
int main() {
  nub_loop_t loop;
  nub_thread_t thread;
  int r;

  nub_loop_init(&loop);
  nub_thread_create(&loop, &thread);

  /* Attach thread to loop for future use */
  /* TODO(trevnorris): Multiple threads may need to be created, so storing the
   * thread on the loop in this way is not the greatest idea. */
  loop.data = &thread;

  /* Will abort if there are any problems */
  tcp4_static_echo_server(PORT, &loop);

  /* Run the event loop using the libnub wrapper */
  r = nub_loop_run(&loop, UV_RUN_DEFAULT);

  /* Cleanup internally allocated resources */
  nub_loop_dispose(&loop);

  return r;
}


/* Start the TCP4 echo server */
static void tcp4_static_echo_server(int port, nub_loop_t* loop) {
  struct sockaddr_in addr;
  uv_tcp_t* tcp_server;
  int r;

  /* Basics for setting up TCP server */
  r = uv_ip4_addr("0.0.0.0", port, &addr);
  check_error(r, "uv_ip4_addr errored");

  tcp_server = (uv_tcp_t*) malloc(sizeof(*tcp_server));
  assert(NULL != tcp_server);

  r = uv_tcp_init(&loop->uvloop, tcp_server);
  check_error(r, "socket creation error");

  r = uv_tcp_bind(tcp_server, (const struct sockaddr*) &addr, 0);
  check_error(r, "bind error");

  /* Attach loop to server for later retrieval */
  tcp_server->data = loop;

  r = uv_listen((uv_stream_t*)tcp_server, SOMAXCONN, on_connection);
  check_error(r, "listen error");

  fprintf(stderr, "Listening on 127.0.0.1:%i\n", port);
}


/* Handle incoming connections */
static void on_connection(uv_stream_t* server, int status) {
  uv_stream_t* stream;
  nub_loop_t* loop;
  int r;

  check_error(status, "connect error");

  loop = (nub_loop_t*)server->data;

  stream = malloc(sizeof(uv_tcp_t));
  assert(NULL != stream);
  r = uv_tcp_init(&loop->uvloop, (uv_tcp_t*)stream);
  check_error(r, "uv_tcp_init error");

  /* associate server with stream */
  stream->data = server;

  r = uv_accept(server, stream);
  check_error(r, "uv_accept error");

  r = uv_read_start(stream, echo_alloc, after_read);
  check_error(r, "uv_read_start error");
}


/* Allocate memory to echo back back the incoming message */
static void echo_alloc(uv_handle_t* handle,
                       size_t suggested_size,
                       uv_buf_t* buf) {
  buf->base = malloc(suggested_size);
  buf->len = suggested_size;
}


/* After reading data from the connection */
static void after_read(uv_stream_t* handle,
                       ssize_t nread,
                       const uv_buf_t* buf) {
  nub_thread_t* thread;
  after_write_t* nubuf;

  /* Oooh. So ugly, but I'm pretty lazy:P */
  thread =
      (nub_thread_t*) ((nub_loop_t*) ((uv_stream_t*) handle->data)->data)->data;

  /* Setting up the struct necessary to pass all relevant information to the
   * spawned thread so it can take care of echoing back the actual request */
  nubuf = (after_write_t*) malloc(sizeof(*nubuf));
  check_error(NULL == nubuf, "allocation error");

  nubuf->len = buf->len;
  nubuf->base = buf->base;
  nubuf->nread = nread;
  nubuf->handle = handle;

  /* Here is where the push to the spawned thread is made */
  nub_thread_push(thread, thread_after_read, nubuf);
}


/* The only function that utilizes the multi-threaded capabilitis of libnub.
 * Made for simplicity in demonstration. All it will do is handle the
 * incoming request and either close the connection, continue listening or
 * respond back with the users data. */
static void thread_after_read(nub_thread_t* thread, void* arg) {
  write_req_t* wr;
  nub_loop_t* loop;
  after_write_t* nubuf;
  uv_handle_t* server_handle;
  uv_stream_t* handle;
  int r;

  /* Unravelling all the different data types */
  loop = thread->nubloop;
  nubuf = (after_write_t*) arg;
  handle = nubuf->handle;
  server_handle = (uv_handle_t*) handle->data;

  /* Error or EOF. Free resources and close connection */
  if (0 > nubuf->nread) {
    free(nubuf->base);

    /* Event loop critical section to close the connection */
    nub_loop_block(thread);
    uv_close((uv_handle_t*)handle, on_close);
    uv_close(server_handle, on_close);
    nub_loop_resume(thread);

    free(nubuf);
    return;
  }

  /* Everything OK, but nothing read */
  if (0 == nubuf->nread) {
    free(nubuf->base);
    free(nubuf);
    return;
  }

  wr = (write_req_t*) malloc(sizeof(*wr));
  check_error(NULL == wr, "allocation error");
  wr->buf = uv_buf_init(nubuf->base, nubuf->nread);

  /* Event loop critical section to echo back read data */
  nub_loop_block(thread);
  r = uv_write(&wr->req, handle, &wr->buf, 1, after_write);
  nub_loop_resume(thread);

  check_error(r, "uv_write error");
  free(nubuf);
}


static void after_write(uv_write_t* req, int status) {
  write_req_t* wr;

  /* Free the read/write buffer and the request */
  wr = (write_req_t*) req;
  free(wr->buf.base);
  free(wr);

  if (status == 0)
    return;

  fprintf(stderr,
          "uv_write error: %s - %s\n",
          uv_err_name(status),
          uv_strerror(status));
}


/* Also close any open handles */
static void on_close(uv_handle_t* peer) {
  free(peer);
}


/* Quick way to check the return value */
static void check_error(int r, const char* msg) {
  if (r) {
    if (0 > r)
      fprintf(stderr, "%s: %s\n", msg, uv_strerror(r));
    else
      fprintf(stderr, "%s: %i\n", msg, r);
    abort();
  }
}
