Starting from **CloudSync version 0.8.3**, the built-in network layer (based on **libcurl**) can be easily replaced with a custom implementation.

#### How It Works

The CloudSync core library provides an abstraction for network operations used to send and receive data during synchronization. By default, it uses libcurl internally. However, you can disable the built-in libcurl support and provide your own network implementation.

This is useful when:

* You want to reduce dependencies on libcurl.
* You need to integrate CloudSync with platform-native networking (such as NSURLSession on Apple platforms).
* You want to use a different networking stack (e.g. for embedded environments, or custom transports).

#### Steps to Provide a Custom Network Layer

1. **Disable libcurl**

   Define the following preprocessor macro in your build system or project:

   ```c
   #define CLOUDSYNC_OMIT_CURL
   ```

   This will exclude the default libcurl-based implementation from your build.

2. **Include the network interface**

   Include the CloudSync internal header that defines the required network API:

   ```c
   #include "network_private.h"
   ```

3. **Implement the required functions**

   You must provide implementations for the following C functions:

   ```c
   bool network_send_buffer (network_data *data, const char *endpoint, const char *authentication, const void *blob, int blob_size);

   // Sends the provided `blob` (binary data) to the specified `endpoint`, using the given `authentication` token or header.
   // Returns `true` on success, `false` on failure.
   ```

   ```c
   NETWORK_RESULT network_receive_buffer (network_data *data, const char *endpoint, const char *authentication, bool zero_terminated, bool is_post_request, char *json_payload, const char **extra_headers, int nextra_headers);

   // Performs a network request (GET or POST depending on `is_post_request`) to the specified `endpoint`, using the given `authentication` token or header.
   // If `json_payload` is provided, it will be sent as the POST body (for `is_post_request == true`).
   // If `zero_terminated == true`, ensure that the returned buffer is null-terminated.
   // `extra_headers` is an array of `nextra_headers` request-header lines (each formatted as `"Name: value"`) appended to the standard headers (`Authorization`, `X-CloudSync-Org`, `Content-Type`). Pass `NULL, 0` for none. Used by call sites to send `X-CloudSync-Version` on every cloudsync API call, `X-CloudSync-Capabilities: check-status-response, check-chunks` on calls to the `/check` endpoint, and `X-CloudSync-Capabilities: apply-defer` on calls to the `/apply` endpoint.
   // Returns a `NETWORK_RESULT` enum value indicating success, error, or timeout.
   ```

#### Requirements for `network_receive_buffer`

The core relies on these behaviors, so a custom implementation must provide them:

* **Forward every `extra_headers` line unchanged.** The capability headers tell the server which responses the client understands. If `apply-defer` is dropped from `/apply` requests, the server never defers that client's applies and answers with a retryable error when the database is busy.
* **Treat every `2xx` status as success, including `202 Accepted`, and return its body.** The server answers `/apply` with `202` when it has accepted the changes but not yet applied them, and `/check` with `202` while a download is being prepared. The body of a `202` carries the sync status the core needs, so it must reach the caller like the body of a `200`.
* **Treat `4xx` and `5xx` as errors.** Do not retry them inside the transport: the core keeps its send checkpoint and the next sync call re-sends.

#### Platform-Specific Notes

* On **macOS**, **iOS**, and **iPadOS**, a native Objective-C implementation is already provided: `network.m`.
  You can use this as a reference or directly include it in your build if you want to use Apple's networking APIs (NSURLSession).

#### Example Use Case

To integrate CloudSync with your own networking stack (e.g. based on `libhttp`, `Boost::Beast`, or a proprietary stack), implement the required functions above and link your network code instead of libcurl.
