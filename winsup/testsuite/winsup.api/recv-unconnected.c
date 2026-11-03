#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static void
fail (const char *what)
{
  perror (what);
  exit (EXIT_FAILURE);
}

static void
check_unconnected_recv (int nonblocking)
{
  int fd = socket (AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    fail ("socket");

  if (nonblocking)
    {
      int flags = fcntl (fd, F_GETFL);
      if (flags < 0 || fcntl (fd, F_SETFL, flags | O_NONBLOCK) < 0)
	fail ("fcntl(O_NONBLOCK)");
    }

  char byte;
  /* Keep a broken blocking receive from hanging the test. */
  alarm (3);
  errno = 0;
  ssize_t result = recv (fd, &byte, sizeof byte, 0);
  int error = errno;
  alarm (0);
  if (result != -1 || error != ENOTCONN)
    {
      fprintf (stderr, "recv(%s): result=%ld errno=%d, expected ENOTCONN\n",
	       nonblocking ? "nonblocking" : "blocking", (long) result, error);
      exit (EXIT_FAILURE);
    }

  if (close (fd) < 0)
    fail ("close");
}

static int
create_listener (struct sockaddr_in *address)
{
  int fd = socket (AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    fail ("socket(listener)");

  memset (address, 0, sizeof *address);
  address->sin_family = AF_INET;
  address->sin_addr.s_addr = htonl (INADDR_LOOPBACK);
  if (bind (fd, (struct sockaddr *) address, sizeof *address) < 0)
    fail ("bind");
  if (listen (fd, 1) < 0)
    fail ("listen");

  socklen_t address_len = sizeof *address;
  if (getsockname (fd, (struct sockaddr *) address, &address_len) < 0)
    fail ("getsockname");
  return fd;
}

static void
check_forked_connection (void)
{
  struct sockaddr_in address;
  int listener = create_listener (&address);
  /* Create before fork so the child's connect is visible to the parent. */
  int client = socket (AF_INET, SOCK_STREAM, 0);
  if (client < 0)
    fail ("socket(client)");

  alarm (10);
  pid_t child = fork ();
  if (child < 0)
    fail ("fork");
  if (!child)
    {
      close (listener);
      if (connect (client, (struct sockaddr *) &address, sizeof address) < 0)
	_exit (EXIT_FAILURE);
      _exit (EXIT_SUCCESS);
    }

  int peer = accept (listener, NULL, NULL);
  if (peer < 0)
    fail ("accept");

  int status;
  if (waitpid (child, &status, 0) < 0)
    fail ("waitpid");
  if (!WIFEXITED (status) || WEXITSTATUS (status) != EXIT_SUCCESS)
    {
      fprintf (stderr, "child connect failed\n");
      exit (EXIT_FAILURE);
    }

  if (send (peer, "x", 1, 0) != 1)
    fail ("send(to inherited socket)");
  char byte;
  ssize_t result = recv (client, &byte, sizeof byte, 0);
  if (result != 1 || byte != 'x')
    {
      fprintf (stderr, "recv(after child connect): result=%ld errno=%d\n",
	       (long) result, errno);
      exit (EXIT_FAILURE);
    }

  /* Exercise recv() on the socket returned by accept(), too. */
  if (send (client, "y", 1, 0) != 1)
    fail ("send(to accepted socket)");
  result = recv (peer, &byte, sizeof byte, 0);
  if (result != 1 || byte != 'y')
    {
      fprintf (stderr, "recv(accepted socket): result=%ld errno=%d\n",
	       (long) result, errno);
      exit (EXIT_FAILURE);
    }
  alarm (0);

  if (close (peer) < 0 || close (client) < 0 || close (listener) < 0)
    fail ("close");
}

int
main (void)
{
  check_unconnected_recv (0);
  check_unconnected_recv (1);
  check_forked_connection ();
  return 0;
}
