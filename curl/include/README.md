<!--
Copyright (C) Daniel Stenberg, <daniel@haxx.se>, et al.

SPDX-License-Identifier: curl
-->

# include

Public include files for libcurl, external users.

They are all placed in the curl subdirectory here for better fit in any kind of
environment. You must include files from here using...

    #include <curl/curl.h>

... style and point the compiler's include path to the directory holding the
curl subdirectory. It makes it more likely to survive future modifications.

The public curl include files can be shared freely between different platforms
and different architectures.

# build

Curl must be statically compiled, and to keep the library as small as possible, we need to remove all the not-needed extra functionalities.

The following configuration is based on curl-8.11.0
Download and extract curl-8.11.0

```bash

    cd curl-8.11.0

    ./configure --with-secure-transport --without-libpsl --disable-alt-svc --disable-ares --disable-cookies --disable-basic-auth --disable-digest-auth --disable-kerberos-auth --disable-negotiate-auth --disable-aws --disable-dateparse --disable-dnsshuffle --disable-doh --disable-form-api --disable-hsts --disable-ipv6 --disable-libcurl-option --disable-manual --disable-mime --disable-netrc --disable-ntlm --disable-ntlm-wb --disable-progress-meter --disable-proxy --disable-pthreads -disable-socketpair --disable-threaded-resolver --disable-tls-srp --disable-verbose --disable-versioned-symbols --enable-symbol-hiding --without-brotli --without-zstd --without-libidn2 --without-librtmp --without-zlib --without-nghttp2 --without-ngtcp2 --disable-shared --disable-ftp --disable-file --disable-ipfs --disable-ldap --disable-ldaps --disable-rtsp --disable-dict --disable-telnet -disable-tftp --disable-pop3 --disable-imap --disable-smb -disable-smtp --disable-gopher --disable-mqtt --disable-docs --enable-static

    make
    open lib/.libs

```