//
//  network.m
//  cloudsync_network_test
//
//  Created by Marco Bambini on 23/05/25.
//

#import <Foundation/Foundation.h>
#include "network.h"
#include "netword_private.h"

/*
 bool network_compute_endpoints (sqlite3_context *context, network_data *data, const char *conn_string) {
     // compute endpoints
     bool result = false;
     
     char *scheme = NULL;
     char *host = NULL;
     char *port = NULL;
     char *database = NULL;
     char *query = NULL;
     
     char *authentication = NULL;
     char *check_endpoint = NULL;
     char *upload_endpoint = NULL;
     
     char *conn_string_https = NULL;
     
     CURLUcode rc = CURLUE_OUT_OF_MEMORY;
     CURLU *url = curl_url();
     if (!url) goto finalize;
     
     conn_string_https = cloudsync_string_replace_prefix(conn_string, "sqlitecloud://", "https://");
     
     // set URL: https://UUID.g5.sqlite.cloud:443/chinook.sqlite?apikey=hWDanFolRT9WDK0p54lufNrIyfgLZgtMw6tb6fbPmpo
     rc = curl_url_set(url, CURLUPART_URL, conn_string_https, 0);
     if (rc != CURLE_OK) goto finalize;
     
     // https (MANDATORY)
     rc = curl_url_get(url, CURLUPART_SCHEME, &scheme, 0);
     if (rc != CURLE_OK) goto finalize;
     
     // UUID.g5.sqlite.cloud (MANDATORY)
     rc = curl_url_get(url, CURLUPART_HOST, &host, 0);
     if (rc != CURLE_OK) goto finalize;
     
     // 443 (OPTIONAL)
     rc = curl_url_get(url, CURLUPART_PORT, &port, 0);
     if (rc != CURLE_OK && rc != CURLUE_NO_PORT) goto finalize;
     char *port_or_default = port ? port : CLOUDSYNC_DEFAULT_ENDPOINT_PORT;

     // /chinook.sqlite (MANDATORY)
     rc = curl_url_get(url, CURLUPART_PATH, &database, 0);
     if (rc != CURLE_OK) goto finalize;
     
     // apikey=hWDanFolRT9WDK0p54lufNrIyfgLZgtMw6tb6fbPmpo (OPTIONAL)
     rc = curl_url_get(url, CURLUPART_QUERY, &query, 0);
     if (rc != CURLE_OK && rc != CURLUE_NO_QUERY) goto finalize;
     if (query != NULL) {
         char value[MAX_QUERY_VALUE_LEN];
         if (!authentication && extract_query_param(query, "apikey", value, sizeof(value)) == 0) {
             authentication = network_authentication_token("apikey", value);
         }
         if (!authentication && extract_query_param(query, "token", value, sizeof(value)) == 0) {
             authentication = network_authentication_token("token", value);
         }
     }
     
     size_t requested = strlen(scheme) + strlen(host) + strlen(port_or_default) + strlen(CLOUDSYNC_ENDPOINT_PREFIX) + strlen(database) + 64;
     check_endpoint = (char *)cloudsync_memory_zeroalloc(requested);
     upload_endpoint = (char *)cloudsync_memory_zeroalloc(requested);
     if ((!upload_endpoint) || (!check_endpoint)) goto finalize;
     
     snprintf(check_endpoint, requested, "%s://%s:%s/%s%s/%s", scheme, host, port_or_default, CLOUDSYNC_ENDPOINT_PREFIX, database, data->site_id);
     snprintf(upload_endpoint, requested, "%s://%s:%s/%s%s/%s/%s", scheme, host, port_or_default, CLOUDSYNC_ENDPOINT_PREFIX, database, data->site_id, CLOUDSYNC_ENDPOINT_UPLOAD);
     
     result = true;
     
 finalize:
     if (result == false) {
         // store proper result code/message
         if (rc != CURLE_OK) sqlite3_result_error(context, curl_url_strerror(rc), -1);
         sqlite3_result_error_code(context, (rc != CURLE_OK) ? SQLITE_ERROR : SQLITE_NOMEM);
         
         // cleanup memory managed by the extension
         if (authentication) cloudsync_memory_free(authentication);
         if (check_endpoint) cloudsync_memory_free(check_endpoint);
         if (upload_endpoint) cloudsync_memory_free(upload_endpoint);
     }
         
     if (result) {
         data->authentication = authentication;
         data->check_endpoint = check_endpoint;
         data->upload_endpoint = upload_endpoint;
     }
     
     // cleanup memory managed by libcurl
     if (scheme) curl_free(scheme);
     if (host) curl_free(host);
     if (port) curl_free(port);
     if (database) curl_free(database);
     if (query) curl_free(query);
     if (url) curl_url_cleanup(url);
     if (conn_string_https && conn_string_https != conn_string) cloudsync_memory_free(conn_string_https);
     
     return result;
 */

void network_buffer_cleanup (void *xdata) {
    if (xdata) CFRelease(xdata);
}

bool network_compute_endpoints (sqlite3_context *context, network_data *data, const char *conn_string) {
    NSString *conn = [NSString stringWithUTF8String:conn_string];
    NSString *conn_string_https = nil;
    
    if ([conn hasPrefix:@"sqlitecloud://"]) {
        conn_string_https = [conn stringByReplacingCharactersInRange:NSMakeRange(0, [@"sqlitecloud://" length]) withString:@"https://"];
    } else {
        conn_string_https = conn;
    }
    
    NSURL *url = [NSURL URLWithString:conn_string_https];
    if (!url) return false;
    
    NSString *scheme = url.scheme;                  // "https"
    if (!scheme) return false;
    NSString *host = url.host;                      // "cn5xiooanz.global3.ryujaz.sqlite.cloud"
    if (!host) return false;
    
    NSString *port = url.port.stringValue;
    NSString *database = url.path;                  // "/chinook-cloudsync.sqlite"
    if (!database) return false;
    
    NSString *query = url.query;                    // "apikey=hWDanFolRT9WDK0p54lufNrIyfgLZgtMw6tb6fbPmpo" (OPTIONAL)
    NSString *authentication = nil;
    
    if (query) {
        NSURLComponents *components = [NSURLComponents componentsWithString:[@"http://dummy?" stringByAppendingString:query]];
        NSArray<NSURLQueryItem *> *items = components.queryItems;
        for (NSURLQueryItem *item in items) {
            // build new token
            // apikey: just write the key for retrocompatibility
            // other keys, like token: add a prefix, i.e. token=
            
            if ([item.name isEqualToString:@"apikey"]) {
                authentication = item.value;
                break;
            }
            if ([item.name isEqualToString:@"token"]) {
                authentication = [NSString stringWithFormat:@"%@=%@", item.name, item.value];
                break;
            }
        }
    }
    
    char *site_id = network_data_get_siteid(data);
    char *port_or_default = (port) ? (char *)port.UTF8String : CLOUDSYNC_DEFAULT_ENDPOINT_PORT;
    
    NSString *check_endpoint = [NSString stringWithFormat:@"%s://%s:%s/%s%s/%s", scheme.UTF8String, host.UTF8String, port_or_default, CLOUDSYNC_ENDPOINT_PREFIX, database.UTF8String, site_id];
    NSString *upload_endpoint = [NSString stringWithFormat: @"%s://%s:%s/%s%s/%s/%s", scheme.UTF8String, host.UTF8String, port_or_default, CLOUDSYNC_ENDPOINT_PREFIX, database.UTF8String, site_id, CLOUDSYNC_ENDPOINT_UPLOAD];
    
    return network_data_set_endpoints(data, (char *)authentication.UTF8String, (char *)check_endpoint.UTF8String, (char *)upload_endpoint.UTF8String, true);
}

bool network_send_buffer(network_data *data, const char *endpoint, const char *authentication, const void *blob, int blob_size) {
    NSString *urlString = [NSString stringWithUTF8String:endpoint];
    NSURL *url = [NSURL URLWithString:urlString];
    if (!url) return false;

    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
    [request setHTTPMethod:@"PUT"];
    [request setValue:@"application/octet-stream" forHTTPHeaderField:@"Content-Type"];
    [request setValue:@"text/plain" forHTTPHeaderField:@"Accept"];

    if (authentication && authentication[0] != '\0') {
        NSString *authString = [NSString stringWithFormat:@"Bearer %s", authentication];
        [request setValue:authString forHTTPHeaderField:@"Authorization"];
    }

    NSData *bodyData = [NSData dataWithBytes:blob length:blob_size];
    [request setHTTPBody:bodyData];

    __block bool success = false;
    dispatch_semaphore_t sema = dispatch_semaphore_create(0);

    NSURLSessionConfiguration *config = [NSURLSessionConfiguration ephemeralSessionConfiguration];
    NSURLSession *session = [NSURLSession sessionWithConfiguration:config];

    NSURLSessionDataTask *task = [session dataTaskWithRequest:request
                                            completionHandler:^(NSData * _Nullable data,
                                                                NSURLResponse * _Nullable response,
                                                                NSError * _Nullable error) {
        if (!error && [response isKindOfClass:[NSHTTPURLResponse class]]) {
            NSInteger statusCode = [(NSHTTPURLResponse *)response statusCode];
            success = (statusCode >= 200 && statusCode < 300);
        }
        dispatch_semaphore_signal(sema);
    }];

    [task resume];
    dispatch_semaphore_wait(sema, DISPATCH_TIME_FOREVER);

    return success;
}


NETWORK_RESULT network_receive_buffer(network_data *data, const char *endpoint, const char *authentication, bool zero_terminated, bool is_post_request, char *json_payload, const char *custom_header) {
    
    NSString *urlString = [NSString stringWithUTF8String:endpoint];
    NSURL *url = [NSURL URLWithString:urlString];
    if (!url) {
        NETWORK_RESULT result = {};
        NSString *msg = [NSString stringWithCString:"Malformed URL" encoding:NSUTF8StringEncoding];
        result.code = CLOUDSYNC_NETWORK_ERROR;
        result.buffer = (char *)msg.UTF8String;
        result.xdata = (void *)CFBridgingRetain(msg);
        result.xfree = network_buffer_cleanup;
        return result;
    }

    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
    request.HTTPMethod = (json_payload || is_post_request) ? @"POST" : @"GET";

    if (custom_header) {
        NSString *header = [NSString stringWithUTF8String:custom_header];
        NSArray<NSString *> *parts = [header componentsSeparatedByString:@": "];
        if (parts.count == 2) {
            [request setValue:parts[1] forHTTPHeaderField:parts[0]];
        }
    }

    if (authentication) {
        NSString *authString = [NSString stringWithFormat:@"Bearer %s", authentication];
        [request setValue:authString forHTTPHeaderField:@"Authorization"];
    }

    if (json_payload) {
        [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
        NSData *jsonData = [NSData dataWithBytes:json_payload length:strlen(json_payload)];
        request.HTTPBody = jsonData;
    } else if (is_post_request) {
        request.HTTPBody = [NSData data]; // empty POST
    }

    __block NSData *responseData = nil;
    __block NSError *responseError = nil;
    __block NSInteger statusCode = 0;

    dispatch_semaphore_t sema = dispatch_semaphore_create(0);

    NSURLSession *session = [NSURLSession sharedSession];
    NSURLSessionDataTask *task = [session dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        responseData = data;
        responseError = error;
        if ([response isKindOfClass:[NSHTTPURLResponse class]]) {
            statusCode = [(NSHTTPURLResponse *)response statusCode];
        }
        dispatch_semaphore_signal(sema);
    }];

    [task resume];
    dispatch_semaphore_wait(sema, DISPATCH_TIME_FOREVER);

    if (!responseError && (statusCode >= 200 && statusCode < 300)) {
        size_t len = [responseData length];
        // check if OK should be returned
        if (len == 0) return (NETWORK_RESULT){CLOUDSYNC_NETWORK_OK, NULL, 0, NULL, NULL};
        
        // otherwise return a buffer
        NETWORK_RESULT result = {};
        result.code = CLOUDSYNC_NETWORK_BUFFER;
        if (zero_terminated) {
            NSString *utf8String = [[NSString alloc] initWithData:responseData encoding:NSUTF8StringEncoding];
            result.buffer = (char *)utf8String.UTF8String;
            result.xdata = (void *)CFBridgingRetain(utf8String);
        } else {
            result.buffer = (char *)responseData.bytes;
            result.xdata = (void *)CFBridgingRetain(responseData);
        }
        result.blen = len;
        result.xfree = network_buffer_cleanup;
        return result;
    }
    
    // return error
    NETWORK_RESULT result = {};
    NSString *msg = (responseError) ? [responseError localizedDescription] : [NSString stringWithCString:"Unknown network URL" encoding:NSUTF8StringEncoding];
    result.code = CLOUDSYNC_NETWORK_ERROR;
    result.buffer = (char *)msg.UTF8String;
    result.xdata = (void *)CFBridgingRetain(msg);
    result.xfree = network_buffer_cleanup;
    result.blen = (responseError) ? (size_t)responseError.code : (size_t)statusCode;
    return result;
}
