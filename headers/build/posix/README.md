# headers/build/posix — POSIX headers for host builds of kernel network code

These headers exist so that network stack code (`src/add-ons/kernel/network/`)
can be compiled for the **build host**, as `src/tests/system/network/tcp_shell`
does for its `BuildPlatformTest` variant.

The rest of `headers/build/` adapts Haiku's *OS* API to a host libc. The
network stack additionally depends on Haiku's *POSIX* network types, and there
the host libc is not merely incomplete but incompatible: Haiku keeps the BSD
`sa_len`/`ss_len`/`sin_len` length field in every socket address, glibc does
not, and the stack stores and compares those fields constantly. A host build
that used glibc's `struct sockaddr` would not be testing the same code.

So each header here redirects to Haiku's own, out of `headers/posix`:

	#include <../posix/netinet/in.h>

This is the idiom the rest of `headers/build` already uses (see
`headers/build/os/kernel/OS.h`): `headers/build` comes first on the include
path, so the `..` resolves against it and lands in `headers/posix`.

Putting `headers/posix` itself on the include path is what we must *not* do: it
would also shadow `<stdio.h>`, `<stdlib.h>` and friends with Haiku's
declarations of a libroot that is not the libc we link against. The list of
redirected headers is therefore deliberately an allowlist — anything not named
here still comes from the host, which is the whole point.

`sys/types.h` is the one exception to the redirect pattern: it chains to the
host's with `#include_next` and then adds only the Haiku types the kernel
headers expect to find there (`addr_t`). Redirecting it outright would drag in
Haiku's view of every fundamental type.
