/*
 * This file is part of Cockpit.
 *
 * Copyright (C) 2019 Red Hat, Inc.
 *
 * Cockpit is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * Cockpit is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Cockpit; If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <gnutls/x509.h>

#include "server.h"
#include "common/cockpittest.h"

#define SOCKET_ACTIVATION_HELPER BUILDDIR "/socket-activation-helper"
#define COCKPIT_WS BUILDDIR "/cockpit-ws"
#define CERTFILE SRCDIR "/src/bridge/mock-server.crt"
#define KEYFILE SRCDIR "/src/bridge/mock-server.key"
#define CERTKEYFILE SRCDIR "/src/ws/mock_cert"
#define CERTCHAINKEYFILE SRCDIR "/test/verify/files/cert-chain.cert"

#define CLIENT_CERTFILE SRCDIR "/src/bridge/mock-client.crt"
#define CLIENT_KEYFILE SRCDIR "/src/bridge/mock-client.key"
#define CLIENT_EXPIRED_CERTFILE SRCDIR "/src/bridge/mock-client-expired.crt"

const unsigned server_port = 9123;

typedef struct {
  gchar *ws_socket_dir;
  GPid ws_spawner;
  struct sockaddr_in server_addr;
} TestCase;

typedef struct {
  const char *certfile;
  const char *keyfile;
  enum ClientCertMode client_certs;
} TestFixture;

static const TestFixture fixture_separate_crt_key = {
  .certfile = CERTFILE,
  .keyfile = KEYFILE,
};

static const TestFixture fixture_separate_crt_key_client_cert = {
  .certfile = CERTFILE,
  .keyfile = KEYFILE,
  .client_certs = CERT_REQUEST,
};

static const TestFixture fixture_combined_crt_key = {
  .certfile = CERTKEYFILE,
};

static const TestFixture fixture_cert_chain = {
  .certfile = CERTCHAINKEYFILE,
};

/* for forking test cases, where server's SIGCHLD handling gets in the way */
static void
block_sigchld (void)
{
  const struct sigaction child_action = { .sa_handler = SIG_DFL };
  g_assert_cmpint (sigaction (SIGCHLD, &child_action, NULL), ==, 0);
}

static int
do_connect (TestCase *tc)
{
  int fd = socket (AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  g_assert_cmpint (fd, >, 0);
  if (connect (fd, (struct sockaddr *) &tc->server_addr, sizeof (tc->server_addr)) < 0)
    {
      close (fd);
      return -errno;
    }
  else
    {
      return fd;
    }
}

static void
send_request (int fd, const char *request)
{
  g_assert_cmpint (fd, >, 0);
  g_assert_cmpint (write (fd, request, strlen (request)), ==, strlen (request));
}

static const char*
recv_reply (int fd, char *buf, size_t buflen)
{
  ssize_t len;

  len = recv (fd, buf, buflen - 1, MSG_DONTWAIT);
  close (fd);
  g_assert_cmpint (len, >=, 100);
  buf[len] = '\0'; /* so that we can use string functions on it */

  return buf;
}

static const char*
do_request (TestCase *tc, const char *request)
{
  static char buf[4096];
  int fd = do_connect (tc);

  send_request (fd, request);
  /* wait until data is available */
  for (int timeout = 0; timeout < 10 && recv (fd, buf, 100, MSG_PEEK | MSG_DONTWAIT) < 100; ++timeout)
    server_poll_event (1000);

  return recv_reply (fd, buf, sizeof (buf));
}

static void
assert_http (TestCase *tc)
{
  const char *res = do_request (tc, "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n");
  /* This succeeds (200 OK) when building in-tree, but fails with dist-check due to missing doc root */
  if (strstr (res, "200 OK"))
    {
      cockpit_assert_strmatch (res, "HTTP/1.1 200 OK\r\n"
                                    "Content-Type: text/html\r\n"
                                    "Content-Security-Policy: connect-src 'self' http://localhost ws://localhost;*");
    }
  else
    {
      cockpit_assert_strmatch (res, "HTTP/1.1 404 Not Found\r\nContent-Type: text/html*");
    }
}

static void
assert_https_outcome (TestCase *tc,
                      const char *client_crt,
                      const char *client_key,
                      unsigned expected_server_certs,
                      bool expect_tls_failure)
{
  pid_t pid;
  int status = -1;

  block_sigchld ();

  /* do the connection in a subprocess, as gnutls_handshake is synchronous */
  pid = fork ();
  if (pid < 0)
    g_error ("failed to fork: %m");
  if (pid == 0)
    {
      const char request[] = "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n";
      char buf[4096];
      gnutls_session_t session;
      gnutls_certificate_credentials_t xcred;
      const gnutls_datum_t *server_certs;
      unsigned server_certs_len;
      ssize_t len;
      int ret;
      int fd = do_connect (tc);

      g_assert_cmpint (fd, >, 0);

      g_assert_cmpint (gnutls_init (&session, GNUTLS_CLIENT), ==, GNUTLS_E_SUCCESS);
      gnutls_transport_set_int (session, fd);
      g_assert_cmpint (gnutls_set_default_priority (session), ==, GNUTLS_E_SUCCESS);
      gnutls_handshake_set_timeout(session, 5000);
      g_assert_cmpint (gnutls_certificate_allocate_credentials (&xcred), ==, GNUTLS_E_SUCCESS);
      g_assert_cmpint (gnutls_certificate_set_x509_system_trust (xcred), >=, 0);
      if (client_crt)
        {
          g_assert (client_key);
          g_assert_cmpint (gnutls_certificate_set_x509_key_file (xcred, client_crt, client_key, GNUTLS_X509_FMT_PEM),
                           ==, GNUTLS_E_SUCCESS);;
        }
      g_assert_cmpint (gnutls_credentials_set (session, GNUTLS_CRD_CERTIFICATE, xcred), ==, GNUTLS_E_SUCCESS);

      ret = gnutls_handshake (session);
      if (ret != GNUTLS_E_SUCCESS)
        {
          if (expect_tls_failure)
            exit (0);
          else
            g_error ("Handshake failed: %s", gnutls_strerror (ret));
        }

      /* check server certificate */
      server_certs = gnutls_certificate_get_peers (session, &server_certs_len);
      g_assert (server_certs);
      g_assert_cmpuint (server_certs_len, ==, expected_server_certs);

      /* send request, read response */
      len = gnutls_record_send (session, request, sizeof (request));
      if (len < 0 && expect_tls_failure)
        exit (0);
      g_assert_cmpint (len, ==, sizeof (request));

      len = gnutls_record_recv (session, buf, sizeof (buf) - 1);
      if (len < 0 && expect_tls_failure)
        exit (0);
      g_assert_cmpint (len, >=, 100);
      g_assert_cmpint (len, <, sizeof (buf) - 1);

      buf[len] = '\0'; /* so that we can use string functions on it */
      /* This succeeds (200 OK) when building in-tree, but fails with dist-check due to missing doc root */
      if (strstr (buf, "200 OK"))
        {
          cockpit_assert_strmatch (buf, "HTTP/1.1 200 OK\r\n"
                                        "Content-Type: text/html\r\n"
                                        "Content-Security-Policy: connect-src 'self' https://localhost wss://localhost;*");
        }
      else
        {
          cockpit_assert_strmatch (buf, "HTTP/1.1 404 Not Found\r\nContent-Type: text/html*");
        }

      g_assert_cmpint (gnutls_bye (session, GNUTLS_SHUT_RDWR), ==, GNUTLS_E_SUCCESS);

      g_assert_false (expect_tls_failure);

      close (fd);
      exit (0);
    }

  for (int retry = 0; retry < 100 && waitpid (pid, &status, WNOHANG) <= 0; ++retry)
    server_poll_event (50);
  g_assert_cmpint (status, ==, 0);
}

static void
assert_https (TestCase *tc, const char *client_crt, const char *client_key, unsigned expected_server_certs)
{
  assert_https_outcome (tc, client_crt, client_key, expected_server_certs, false);
}

static void
setup (TestCase *tc, gconstpointer data)
{
  const TestFixture *fixture = data;
  g_autoptr(GError) error = NULL;

  tc->ws_socket_dir = g_dir_make_tmp ("server.wssock.XXXXXX", NULL);
  g_assert (tc->ws_socket_dir);

  gchar* sah_argv[] = { SOCKET_ACTIVATION_HELPER, COCKPIT_WS, tc->ws_socket_dir, NULL };
  if (!g_spawn_async (NULL, sah_argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &tc->ws_spawner, &error))
    g_error ("Failed to spawn " SOCKET_ACTIVATION_HELPER ": %s", error->message);

  /* wait until socket activation helper is ready */
  int socket_dir_fd = open (tc->ws_socket_dir, O_RDONLY | O_DIRECTORY);
  for (int retry = 0; retry < 200; ++retry)
    {
      if (faccessat (socket_dir_fd, "ready", F_OK, 0) == 0)
        break;
      g_usleep (10000);
    }
  close (socket_dir_fd);

  server_init (tc->ws_socket_dir,
               server_port,
               fixture ? fixture->certfile : NULL,
               fixture ? fixture->keyfile : NULL,
               fixture ? fixture->client_certs : CERT_NONE);
  tc->server_addr.sin_family = AF_INET;
  tc->server_addr.sin_port = htons (server_port);
  tc->server_addr.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
}

static void
teardown (TestCase *tc, gconstpointer data)
{
  server_cleanup ();
  g_assert_cmpint (kill (tc->ws_spawner, SIGTERM), ==, 0);
  g_assert_cmpint (waitpid (tc->ws_spawner, NULL, 0), ==, tc->ws_spawner);

  /* all children got cleaned up */
  g_assert_cmpint (wait (NULL), ==, -1);
  g_assert_cmpint (errno, ==, ECHILD);
  /* connection should fail */
  g_assert_cmpint (do_connect (tc), ==, -ECONNREFUSED);
  g_unsetenv ("COCKPIT_WS_PROCESS_IDLE");

  int socket_dir_fd = open (tc->ws_socket_dir, O_RDONLY | O_DIRECTORY);
  g_assert_cmpint (unlinkat (socket_dir_fd, "http.sock", 0), ==, 0);
  g_assert_cmpint (unlinkat (socket_dir_fd, "http-redirect.sock", 0), ==, 0);
  g_assert_cmpint (unlinkat (socket_dir_fd, "https.sock", 0), ==, 0);
  g_assert_cmpint (unlinkat (socket_dir_fd, "ready", 0), ==, 0);
  close (socket_dir_fd);
  g_assert_cmpint (g_rmdir (tc->ws_socket_dir), ==, 0);
  g_free (tc->ws_socket_dir);
}

static void
test_no_tls_con_shutdown (TestCase *tc, gconstpointer data)
{
  assert_http (tc);

  /* let the server process "peer has closed connection" */
  for (int retries = 0; retries < 10 && server_num_connections () == 1; ++retries)
    server_run (100);
  g_assert_cmpuint (server_num_connections (), ==, 0);
}

static void
test_no_tls_many_serial (TestCase *tc, gconstpointer data)
{
  for (int i = 0; i < 20; ++i)
    assert_http (tc);
}

static void
test_no_tls_many_parallel (TestCase *tc, gconstpointer data)
{
  int i;

  block_sigchld ();

  for (i = 0; i < 20; ++i)
    {
      pid_t pid = fork ();
      if (pid < 0)
        g_error ("failed to fork: %m");

      if (pid > 0)
        continue;

      /* child */
      char buf[4096];
      int fd = do_connect (tc);

      server_cleanup ();

      send_request (fd, "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n");
      /* wait until data is available */
      for (int timeout = 0; timeout < 10 && recv (fd, buf, 100, MSG_PEEK | MSG_DONTWAIT) < 100; ++timeout)
        sleep (1);
      recv_reply (fd, buf, sizeof (buf));
      /* This succeeds (200 OK) when building in-tree, but fails with dist-check due to missing doc root */
      if (strstr (buf, "200 OK"))
        cockpit_assert_strmatch (buf, "HTTP/1.1 200 OK*");
      else
        cockpit_assert_strmatch (buf, "HTTP/1.1 404 Not Found*");
      exit (0);
    }

  /* wait until all i child processes have finished */
  while (i > 0)
  {
    int status;
    int r = waitpid (-1, &status, WNOHANG);
    g_assert_cmpint (r, >=, 0);
    if (r == 0)
      {
        server_poll_event (50);
      }
    else
      {
        g_assert_cmpint (status, ==, 0);
        --i;
      }
  }
}

static void
test_no_tls_redirect (TestCase *tc, gconstpointer data)
{
  /* without TLS support it should not redirect */
  const char *res = do_request (tc, "GET / HTTP/1.0\r\nHost: some.remote:1234\r\n\r\n");
  /* This succeeds (200 OK) when building in-tree, but fails with dist-check due to missing doc root */
  if (strstr (res, "200 OK"))
    cockpit_assert_strmatch (res, "HTTP/1.1 200 OK*");
  else
    cockpit_assert_strmatch (res, "HTTP/1.1 404 Not Found*");
}

static void
test_tls_no_client_cert (TestCase *tc, gconstpointer data)
{
  assert_https (tc, NULL, NULL, 1);
}

static void
test_tls_no_server_cert (TestCase *tc, gconstpointer data)
{
  assert_http (tc);
  assert_https_outcome (tc, NULL, NULL, 0, true);
  assert_http (tc);
}

static void
test_tls_redirect (TestCase *tc, gconstpointer data)
{
  /* with TLS support it should redirect */
  const char *res = do_request (tc, "GET / HTTP/1.0\r\nHost: some.remote:1234\r\n\r\n");
  cockpit_assert_strmatch (res, "HTTP/1.1 301 Moved Permanently*");
}

static void
test_tls_client_cert (TestCase *tc, gconstpointer data)
{
  assert_https (tc, CLIENT_CERTFILE, CLIENT_KEYFILE, 1);
  /* no-cert case is handled by separate ws */
  assert_https (tc, NULL, NULL, 1);
  assert_https (tc, CLIENT_CERTFILE, CLIENT_KEYFILE, 1);
}

static void
test_tls_client_cert_disabled (TestCase *tc, gconstpointer data)
{
  assert_https (tc, CLIENT_CERTFILE, CLIENT_KEYFILE, 1);
  /* no-cert case is handled by same ws, as client certs are disabled server-side */
  assert_https (tc, NULL, NULL, 1);
}

static void
test_tls_client_cert_expired (TestCase *tc, gconstpointer data)
{
  /* expect_tls_failure==true only does a coarse-grained check that the request
   * fails anywhere during handshake or the first send/recv. GnuTLS 3.6.4
   * introduces TLS 1.3 by default, which has only a two-step handshake: that
   * does not pick up the server's late failing handshake from the verify
   * function, only the next read/write attempt does */
  assert_https_outcome (tc, CLIENT_EXPIRED_CERTFILE, CLIENT_KEYFILE, 1, true);
}

static void
test_tls_cert_chain (TestCase *tc, gconstpointer data)
{
  /* CERTCHAINKEYFILE has two certs */
  assert_https (tc, NULL, NULL, 2);
}

static void
test_mixed_protocols (TestCase *tc, gconstpointer data)
{
  assert_https (tc, NULL, NULL, 1);
  assert_http (tc);
  assert_https (tc, NULL, NULL, 1);
  assert_http (tc);
}

static void
test_run_idle (TestCase *tc, gconstpointer data)
{
  /* exits after idle without any connections */
  server_run (100);

  /* exits after idle after processing an event */
  assert_http (tc);
  server_run (100);
}

int
main (int argc, char *argv[])
{
  cockpit_test_init (&argc, &argv);

  g_test_add ("/server/no-tls/process-connection-shutdown", TestCase, NULL,
              setup, test_no_tls_con_shutdown, teardown);
  g_test_add ("/server/no-tls/many-serial", TestCase, NULL,
              setup, test_no_tls_many_serial, teardown);
  g_test_add ("/server/no-tls/many-parallel", TestCase, NULL,
              setup, test_no_tls_many_parallel, teardown);
  g_test_add ("/server/no-tls/redirect", TestCase, NULL,
              setup, test_no_tls_redirect, teardown);
  g_test_add ("/server/tls/no-client-cert", TestCase, &fixture_separate_crt_key,
              setup, test_tls_no_client_cert, teardown);
  g_test_add ("/server/tls/client-cert", TestCase, &fixture_separate_crt_key_client_cert,
              setup, test_tls_client_cert, teardown);
  g_test_add ("/server/tls/client-cert-disabled", TestCase, &fixture_separate_crt_key,
              setup, test_tls_client_cert_disabled, teardown);
  g_test_add ("/server/tls/client-cert-expired", TestCase, &fixture_separate_crt_key_client_cert,
              setup, test_tls_client_cert_expired, teardown);
  g_test_add ("/server/tls/combined-server-cert-key", TestCase, &fixture_combined_crt_key,
              setup, test_tls_no_client_cert, teardown);
  g_test_add ("/server/tls/cert-chain", TestCase, &fixture_cert_chain,
              setup, test_tls_cert_chain, teardown);
  g_test_add ("/server/tls/no-server-cert", TestCase, NULL,
              setup, test_tls_no_server_cert, teardown);
  g_test_add ("/server/tls/redirect", TestCase, &fixture_combined_crt_key,
              setup, test_tls_redirect, teardown);
  g_test_add ("/server/mixed-protocols", TestCase, &fixture_separate_crt_key,
              setup, test_mixed_protocols, teardown);
  g_test_add ("/server/run-idle", TestCase, NULL,
              setup, test_run_idle, teardown);

  return g_test_run ();
}