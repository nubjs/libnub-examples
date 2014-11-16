#include "nub.h"
#include "uv.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define PORT 7856

typedef struct {
  uv_write_t req;
  uv_buf_t buf;
} write_req_t;

/* Forward declarations */
static void tcp4_static_echo_server(int port, nub_loop_t* loop);
static void on_connection(uv_stream_t* server, int status);
static void echo_alloc(uv_handle_t* handle,
                       size_t suggested_size,
                       uv_buf_t* buf);
static void after_read(uv_stream_t* handle, ssize_t nread, const uv_buf_t* buf);
static void after_write(uv_write_t* req, int status);
static void on_close(uv_handle_t* peer);
static void after_shutdown(uv_shutdown_t* req, int status);
static void check_error(int r, const char* msg);


/* Entry point */
int main() {
  nub_loop_t loop;
  int r;

  nub_loop_init(&loop);

  /* Will abort if there are any problems */
  tcp4_static_echo_server(PORT, &loop);

  /* Run the event loop using the libnub wrapper */
  r = nub_loop_run(&loop, UV_RUN_DEFAULT);

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
  write_req_t* wr;
  uv_shutdown_t* sreq;
  uv_handle_t* server_handle;
  static int server_closed = 0;
  int i;
  int r;

  server_handle = (uv_handle_t*) handle->data;

  if (0 > nread) {
    /* Error or EOF. Free resources and close connection */
    assert(UV_EOF == nread);
    free(buf->base);
    sreq = malloc(sizeof(*sreq));
    r = uv_shutdown(sreq, handle, after_shutdown);
    check_error(r, "uv_shutdown error");
    return;
  }

  if (0 == nread) {
    /* Everything OK, but nothing read. */
    free(buf->base);
    return;
  }

  /**
   * Scan for the letter Q which signals that we should quit the server.
   * If we get QS it means close the stream.
   */
  if (!server_closed) {
    for (i = 0; i < nread; i++) {
      if (buf->base[i] == 'Q') {
        if (i + 1 < nread && buf->base[i + 1] == 'S') {
          free(buf->base);
          uv_close((uv_handle_t*)handle, on_close);
          return;
        } else {
          uv_close(server_handle, on_close);
          server_closed = 1;
        }
      }
    }
  }

  wr = (write_req_t*) malloc(sizeof(*wr));
  assert(NULL != wr);
  wr->buf = uv_buf_init(buf->base, nread);

  r = uv_write(&wr->req, handle, &wr->buf, 1, after_write);
  check_error(r, "uv_write error");
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


/* Make sure to cleanup resources after server shuts down */
static void after_shutdown(uv_shutdown_t* req, int status) {
  uv_close((uv_handle_t*) req->handle, on_close);
  free(req);
}


/* Also close any open handles */
static void on_close(uv_handle_t* peer) {
  free(peer);
}


/* Quick way to check the return value */
static void check_error(int r, const char* msg) {
  if (r) {
    fprintf(stderr, "%s: %i\n", msg, r);
    abort();
  }
}
