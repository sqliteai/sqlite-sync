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
   bool network_compute_endpoints (sqlite3_context *context, network_data *data, const char *conn_string);

   // Parses `conn_string` and fills the `network_data` structure with connection information (e.g. base URL, endpoints, credentials).
   // Returns `true` on success, `false` on error (you can use `sqlite3_result_error` to report errors to SQLite).
   
   ```

   ```c
   bool network_send_buffer (network_data *data, const char *endpoint, const char *authentication, const void *blob, int blob_size);

   // Sends the provided `blob` (binary data) to the specified `endpoint`, using the given `authentication` token or header.
   // Returns `true` on success, `false` on failure.
   ```

   ```c
   NETWORK_RESULT network_receive_buffer (network_data *data, const char *endpoint, const char *authentication, bool zero_terminated, bool is_post_request, char *json_payload, const char *custom_header);

   // Performs a network request (GET or POST depending on `is_post_request`) to the specified `endpoint`, using the given `authentication` token or header.
   // If `json_payload` is provided, it will be sent as the POST body (for `is_post_request == true`).
   // If `zero_terminated == true`, ensure that the returned buffer is null-terminated.
   // Returns a `NETWORK_RESULT` enum value indicating success, error, or timeout.
   ```

#### Platform-Specific Notes

* On **macOS**, **iOS**, and **iPadOS**, a native Objective-C implementation is already provided: `network.m`.
  You can use this as a reference or directly include it in your build if you want to use Apple's networking APIs (NSURLSession).

#### Example Use Case

To integrate CloudSync with your own networking stack (e.g. based on `libhttp`, `Boost::Beast`, or a proprietary stack), implement the required functions above and link your network code instead of libcurl.
