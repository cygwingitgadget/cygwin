/* exec-token.c: check that exec() does not leak process token references
   in the Windows console host.

   This file is part of Cygwin.

   This software is a copyrighted work licensed under the terms of the
   Cygwin license.  Please consult the file "CYGWIN_LICENSE" for
   details.

   See: https://github.com/git-for-windows/git/issues/6471
	https://github.com/microsoft/terminal/issues/20781 */

#include <windows.h>
#include <winternl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ULONG
get_token_refcount (HANDLE token)
{
  ULONG min_refs = (ULONG) -1;
  HANDLE cur = token;

  /* Query up to 2 seconds for conhost to finish any async cleanup. */
  for (int i = 0; i < 20; i++)
    {
      HANDLE next;
      OBJECT_BASIC_INFORMATION info;

      if (!DuplicateHandle (GetCurrentProcess (), cur, GetCurrentProcess (),
			    &next, 0, FALSE,
			    DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE))
	break;
      cur = next;

      if (NtQueryObject (cur, ObjectBasicInformation, &info,
			 sizeof (info), NULL) >= 0)
	{
	  if (info.PointerCount < min_refs)
	    min_refs = info.PointerCount;
	}
      Sleep (100);
    }
  CloseHandle (cur);
  return min_refs;
}

static ULONG
measure_worker (const char *self, const char *action)
{
  SECURITY_ATTRIBUTES sa = { sizeof (sa), NULL, TRUE };
  HANDLE hReady = CreateEventA (&sa, TRUE, FALSE, NULL);
  HANDLE hGo = CreateEventA (&sa, TRUE, FALSE, NULL);
  HANDLE hToken = NULL;
  STARTUPINFOA si;
  memset (&si, 0, sizeof (si));
  si.cb = sizeof (si);
  PROCESS_INFORMATION pi;
  char cmd[MAX_PATH + 64];

  snprintf (cmd, sizeof (cmd), "\"%s\" worker %s %llu %llu",
	    self, action, (unsigned long long) (uintptr_t) hReady,
	    (unsigned long long) (uintptr_t) hGo);

  if (!CreateProcessA (self, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi))
    {
      CloseHandle (hReady);
      CloseHandle (hGo);
      return 0;
    }

  if (WaitForSingleObject (hReady, 10000) != WAIT_OBJECT_0
      || !OpenProcessToken (pi.hProcess, TOKEN_QUERY | TOKEN_DUPLICATE,
			    &hToken))
    {
      SetEvent (hGo);
      WaitForSingleObject (pi.hProcess, 10000);
      CloseHandle (pi.hProcess);
      CloseHandle (pi.hThread);
      CloseHandle (hReady);
      CloseHandle (hGo);
      return 0;
    }

  SetEvent (hGo);
  WaitForSingleObject (pi.hProcess, 10000);
  CloseHandle (pi.hProcess);
  CloseHandle (pi.hThread);
  CloseHandle (hReady);
  CloseHandle (hGo);

  return get_token_refcount (hToken);
}

int
main (int argc, char **argv)
{
  char self[MAX_PATH];

  if (!GetModuleFileNameA (NULL, self, sizeof (self)))
    return 1;

  if (argc == 2 && strcmp (argv[1], "leaf") == 0)
    return 0;

  if (argc == 5 && strcmp (argv[1], "worker") == 0)
    {
      HANDLE hReady = (HANDLE) (uintptr_t) strtoull (argv[3], NULL, 10);
      HANDLE hGo = (HANDLE) (uintptr_t) strtoull (argv[4], NULL, 10);

      SetEvent (hReady);
      WaitForSingleObject (hGo, 10000);
      CloseHandle (hReady);
      CloseHandle (hGo);

      if (strcmp (argv[2], "exec") == 0)
	execl ("/proc/self/exe", "exec-token", "leaf", (char *) NULL);
      return 0;
    }

  if (argc == 3 && strcmp (argv[1], "run") == 0)
    {
      HANDLE hPipe = (HANDLE) (uintptr_t) strtoull (argv[2], NULL, 10);
      ULONG refs_exit = measure_worker (self, "exit");
      ULONG refs_exec = measure_worker (self, "exec");
      DWORD written;

      WriteFile (hPipe, &refs_exit, sizeof (refs_exit), &written, NULL);
      WriteFile (hPipe, &refs_exec, sizeof (refs_exec), &written, NULL);
      return 0;
    }

  /* Main test runner: launch a private console session to test conhost. */
  SECURITY_ATTRIBUTES sa = { sizeof (sa), NULL, TRUE };
  HANDLE hRead, hWrite;

  if (!CreatePipe (&hRead, &hWrite, &sa, 0))
    return 77;

  STARTUPINFOA si;
  memset (&si, 0, sizeof (si));
  si.cb = sizeof (si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi;
  char cmd[MAX_PATH + 64];

  snprintf (cmd, sizeof (cmd), "\"%s\" run %llu", self,
	    (unsigned long long) (uintptr_t) hWrite);

  if (!CreateProcessA (self, cmd, NULL, NULL, TRUE, CREATE_NEW_CONSOLE,
		       NULL, NULL, &si, &pi))
    {
      CloseHandle (hRead);
      CloseHandle (hWrite);
      return 77;
    }
  CloseHandle (hWrite);

  ULONG refs_exit = 0, refs_exec = 0;
  DWORD bytes;

  ReadFile (hRead, &refs_exit, sizeof (refs_exit), &bytes, NULL);
  ReadFile (hRead, &refs_exec, sizeof (refs_exec), &bytes, NULL);
  CloseHandle (hRead);

  WaitForSingleObject (pi.hProcess, 30000);
  CloseHandle (pi.hProcess);
  CloseHandle (pi.hThread);

  if (refs_exit == 0 || refs_exit == (ULONG) -1)
    {
      fprintf (stderr, "Failed to measure token reference count\n");
      return 77;
    }

  printf ("Token pointer count: exit=%lu, exec=%lu\n",
	  (unsigned long) refs_exit, (unsigned long) refs_exec);

  if (refs_exec > refs_exit)
    {
      fprintf (stderr, "FAIL: exec() leaked %lu token reference(s)\n",
	       (unsigned long) (refs_exec - refs_exit));
      return 1;
    }

  printf ("PASS: no token reference leak detected\n");
  return 0;
}
