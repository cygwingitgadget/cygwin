#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#include <windows.h>

enum { FORKS = 100, EVENTS = 1000 };

static void
win32_error (const char *operation)
{
  fprintf (stderr, "%s: Win32 error %lu\n", operation,
	   (unsigned long) GetLastError ());
}

static pid_t
wait_for (pid_t pid, int *status, int options)
{
  pid_t waited;

  do
    waited = waitpid (pid, status, options);
  while (waited < 0 && errno == EINTR);
  return waited;
}

static int
drain_fifo (int fd)
{
  char buffer[4096];
  ssize_t n;

  do
    n = read (fd, buffer, sizeof buffer);
  while (n > 0 || (n < 0 && errno == EINTR));
  if (n < 0 && errno != EAGAIN)
    {
      perror ("read FIFO");
      return 2;
    }
  return 0;
}

static void
writer_churn (const char *path, HANDLE stop)
{
  unsigned long writes = 0;

  for (;;)
    {
      DWORD stopped = WaitForSingleObject (stop, 0);
      if (stopped == WAIT_OBJECT_0)
	{
	  if (!writes)
	    fprintf (stderr, "FIFO writer completed no writes\n");
	  _exit (writes ? 0 : 2);
	}
      if (stopped != WAIT_TIMEOUT)
	{
	  win32_error ("wait for writer stop");
	  _exit (2);
	}

      /* Each open makes the parent's reader thread add a pipe instance. */
      int fd = open (path, O_WRONLY | O_NONBLOCK);
      if (fd < 0)
	{
	  if (errno == EINTR || errno == EAGAIN)
	    continue;
	  perror ("writer open FIFO");
	  _exit (2);
	}
      ssize_t n;
      do
	n = write (fd, "x", 1);
      while (n < 0 && errno == EINTR);
      if (n != 1 && !(n < 0 && errno == EAGAIN))
	{
	  if (n < 0)
	    perror ("writer write FIFO");
	  else
	    fprintf (stderr, "FIFO writer made a short write\n");
	  _exit (2);
	}
      writes += n == 1;
      if (close (fd))
	{
	  perror ("writer close FIFO");
	  _exit (2);
	}
    }
}

static int
check_child (int reader, int writer, HANDLE *events, int iteration)
{
  for (int k = 0; k < EVENTS; k++)
    if (!(events[k] = CreateEventA (NULL, TRUE, FALSE, NULL)))
      {
	win32_error ("create child event");
	return 2;
      }

  if (close (reader) || close (writer))
    {
      perror ("close inherited FIFO");
      return 2;
    }
  for (int k = 0; k < EVENTS; k++)
    {
      DWORD flags;
      if (!GetHandleInformation (events[k], &flags))
	{
	  DWORD error = GetLastError ();
	  fprintf (stderr, "fork %d: event %d lost (Win32 error %lu)\n",
		   iteration, k, (unsigned long) error);
	  return error == ERROR_INVALID_HANDLE ? 1 : 2;
	}
    }
  return 0;
}

int
main (void)
{
  char directory[] = "/tmp/fifo-fork.XXXXXX";
  char path[sizeof directory + sizeof "/fifo"];
  struct utsname runtime;
  SECURITY_ATTRIBUTES attributes = { sizeof attributes, NULL, TRUE };
  HANDLE stop = NULL, *events = NULL;
  pid_t writer = -1;
  int reader = -1, persistent_writer = -1;
  int status, result = 2, fifo_created = 0;

  if (uname (&runtime))
    {
      perror ("uname");
      return 2;
    }
  fprintf (stderr, "Runtime: %s %s %s %s\n", runtime.sysname,
	   runtime.release, runtime.version, runtime.machine);
  if (!mkdtemp (directory))
    {
      perror ("mkdtemp");
      return 2;
    }
  snprintf (path, sizeof path, "%s/fifo", directory);
  if (mkfifo (path, 0600))
    {
      perror ("mkfifo");
      goto out;
    }
  fifo_created = 1;
  stop = CreateEventA (&attributes, TRUE, FALSE, NULL);
  if (!stop)
    {
      win32_error ("create writer stop event");
      goto out;
    }
  reader = open (path, O_RDONLY | O_NONBLOCK);
  if (reader < 0)
    {
      perror ("open FIFO reader");
      goto out;
    }
  persistent_writer = open (path, O_WRONLY);
  if (persistent_writer < 0)
    {
      perror ("open persistent FIFO writer");
      goto out;
    }
  events = malloc (EVENTS * sizeof *events);
  if (!events)
    {
      perror ("allocate child events");
      goto out;
    }
  writer = fork ();
  if (writer < 0)
    {
      perror ("fork FIFO writer");
      goto out;
    }
  if (!writer)
    writer_churn (path, stop);

  for (int i = 0; i < FORKS; i++)
    {
      pid_t waited = wait_for (writer, &status, WNOHANG);
      if (waited)
	{
	  if (waited < 0)
	    perror ("wait for FIFO writer");
	  else
	    {
	      fprintf (stderr, "FIFO writer exited early (status 0x%x)\n",
		       status);
	      writer = -1;
	    }
	  goto out;
	}
      if (drain_fifo (reader))
	goto out;
      pid_t child = fork ();
      if (child < 0)
	{
	  perror ("fork test child");
	  goto out;
	}
      if (!child)
	_exit (check_child (reader, persistent_writer, events, i));
      if (wait_for (child, &status, 0) != child)
	{
	  perror ("wait for test child");
	  goto out;
	}
      if (status)
	{
	  result = WIFEXITED (status) && WEXITSTATUS (status) == 1 ? 1 : 2;
	  fprintf (stderr, "%s: fork %d (child status 0x%x)\n",
		   result == 1 ? "Lost child event" : "Test child failed",
		   i, status);
	  goto out;
	}
    }
  result = 0;

out:
  if (writer > 0)
    {
      if (!SetEvent (stop))
	{
	  win32_error ("stop FIFO writer");
	  result = 2;
	}
      /* A nonblocking writer open can still wait for a free pipe instance. */
      for (;;)
	{
	  if (drain_fifo (reader))
	    result = 2;
	  pid_t waited = wait_for (writer, &status, WNOHANG);
	  if (waited == writer)
	    {
	      if (status)
		{
		  fprintf (stderr, "FIFO writer failed (status 0x%x)\n",
			   status);
		  result = 2;
		}
	      break;
	    }
	  if (waited < 0)
	    {
	      perror ("reap FIFO writer");
	      result = 2;
	      break;
	    }
	  Sleep (1);
	}
    }
  if (reader >= 0 && close (reader))
    {
      perror ("close FIFO reader");
      result = 2;
    }
  if (persistent_writer >= 0 && close (persistent_writer))
    {
      perror ("close persistent FIFO writer");
      result = 2;
    }
  if (stop && !CloseHandle (stop))
    {
      win32_error ("close writer stop event");
      result = 2;
    }
  free (events);
  if (fifo_created && unlink (path))
    {
      perror ("unlink FIFO");
      result = 2;
    }
  if (rmdir (directory))
    {
      perror ("remove FIFO directory");
      result = 2;
    }
  if (!result)
    printf ("OK: %d forks preserved %d events per child\n", FORKS, EVENTS);
  return result;
}
