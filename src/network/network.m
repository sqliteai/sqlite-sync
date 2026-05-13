//
//  network.m
//  cloudsync
//
//  Created by Marco Bambini on 23/05/25.
//

#import <Foundation/Foundation.h>
#include "network.h"
#include "network_private.h"

void network_buffer_cleanup (void *xdata) {
    if (xdata) CFRelease(xdata);
}

bool network_send_buffer(network_data *data, const char *endpoint, const char *authentication, const void *blob, int blob_size) {
#ifdef CLOUDSYNC_NETWORK_TRACE
    double trace_start_ms = network_trace_now_ms();
#endif
    NSString *urlString = [NSString stringWithUTF8String:endpoint];
    NSURL *url = [NSURL URLWithString:urlString];
    if (!url) {
        #ifdef CLOUDSYNC_NETWORK_TRACE
        network_trace_log(data, "PUT", endpoint, 0, CLOUDSYNC_NETWORK_ERROR, 0, network_trace_now_ms() - trace_start_ms);
        #endif
        return false;
    }

    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
    [request setHTTPMethod:@"PUT"];
    [request setValue:@"application/octet-stream" forHTTPHeaderField:@"Content-Type"];
    [request setValue:@"text/plain" forHTTPHeaderField:@"Accept"];

    if (authentication && authentication[0] != '\0') {
        NSString *authString = [NSString stringWithFormat:@"Bearer %s", authentication];
        [request setValue:authString forHTTPHeaderField:@"Authorization"];
    }

    char *org_id = network_data_get_orgid(data);
    if (org_id) {
        [request setValue:[NSString stringWithUTF8String:org_id] forHTTPHeaderField:@CLOUDSYNC_HEADER_ORG];
    }

    NSData *bodyData = [NSData dataWithBytes:blob length:blob_size];
    [request setHTTPBody:bodyData];

    __block bool success = false;
    __block NSInteger statusCode = 0;
    dispatch_semaphore_t sema = dispatch_semaphore_create(0);

    NSURLSessionConfiguration *config = [NSURLSessionConfiguration ephemeralSessionConfiguration];
    NSURLSession *session = [NSURLSession sessionWithConfiguration:config];

    NSURLSessionDataTask *task = [session dataTaskWithRequest:request
                                            completionHandler:^(NSData * _Nullable responseBody,
                                                                NSURLResponse * _Nullable response,
                                                                NSError * _Nullable error) {
        if (!error && [response isKindOfClass:[NSHTTPURLResponse class]]) {
            statusCode = [(NSHTTPURLResponse *)response statusCode];
            success = (statusCode >= 200 && statusCode < 300);
        }
        dispatch_semaphore_signal(sema);
    }];

    [task resume];
    dispatch_semaphore_wait(sema, DISPATCH_TIME_FOREVER);
    [session finishTasksAndInvalidate];

    #ifdef CLOUDSYNC_NETWORK_TRACE
    network_trace_log(data, "PUT", endpoint, (long)statusCode,
                      success ? CLOUDSYNC_NETWORK_OK : CLOUDSYNC_NETWORK_ERROR,
                      success ? (size_t)blob_size : 0,
                      network_trace_now_ms() - trace_start_ms);
    #endif

    return success;
}


NETWORK_RESULT network_receive_buffer(network_data *data, const char *endpoint, const char *authentication, bool zero_terminated, bool is_post_request, char *json_payload, const char **extra_headers, int nextra_headers) {
#ifdef CLOUDSYNC_NETWORK_TRACE
    double trace_start_ms = network_trace_now_ms();
#endif
    const char *method = (json_payload || is_post_request) ? "POST" : "GET";
    
    NSString *urlString = [NSString stringWithUTF8String:endpoint];
    NSURL *url = [NSURL URLWithString:urlString];
    if (!url) {
        NETWORK_RESULT result = {};
        NSString *msg = [NSString stringWithCString:"Malformed URL" encoding:NSUTF8StringEncoding];
        result.code = CLOUDSYNC_NETWORK_ERROR;
        result.buffer = (char *)msg.UTF8String;
        result.xdata = (void *)CFBridgingRetain(msg);
        result.xfree = network_buffer_cleanup;
        #ifdef CLOUDSYNC_NETWORK_TRACE
        network_trace_log(data, method, endpoint, 0, result.code, 0, network_trace_now_ms() - trace_start_ms);
        #endif
        return result;
    }

    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
    request.HTTPMethod = (json_payload || is_post_request) ? @"POST" : @"GET";

    for (int i = 0; i < nextra_headers; i++) {
        NSString *header = [NSString stringWithUTF8String:extra_headers[i]];
        NSRange sep = [header rangeOfString:@": "];
        if (sep.location != NSNotFound) {
            NSString *name = [header substringToIndex:sep.location];
            NSString *value = [header substringFromIndex:sep.location + sep.length];
            [request setValue:value forHTTPHeaderField:name];
        }
    }

    char *org_id = network_data_get_orgid(data);
    if (org_id) {
        [request setValue:[NSString stringWithUTF8String:org_id] forHTTPHeaderField:@CLOUDSYNC_HEADER_ORG];
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
    __block NSString *responseError = nil;
    __block NSInteger statusCode = 0;
    __block NSInteger errorCode = 0;

    dispatch_semaphore_t sema = dispatch_semaphore_create(0);

    NSURLSessionConfiguration *config = [NSURLSessionConfiguration ephemeralSessionConfiguration];
    NSURLSession *session = [NSURLSession sessionWithConfiguration:config];
    NSURLSessionDataTask *task = [session dataTaskWithRequest:request completionHandler:^(NSData *responseBody, NSURLResponse *response, NSError *error) {
        responseData = responseBody;
        if (error) {
            responseError = [error localizedDescription];
            errorCode = [error code];
        }
        if ([response isKindOfClass:[NSHTTPURLResponse class]]) {
            statusCode = [(NSHTTPURLResponse *)response statusCode];
        }
        dispatch_semaphore_signal(sema);
    }];

    [task resume];
    dispatch_semaphore_wait(sema, DISPATCH_TIME_FOREVER);
    [session finishTasksAndInvalidate];

    if (!responseError && (statusCode >= 200 && statusCode < 300)) {
        // check if OK should be returned
        if (responseData == nil || [responseData length] == 0) {
            NETWORK_RESULT result = {CLOUDSYNC_NETWORK_OK, NULL, 0, NULL, NULL};
            #ifdef CLOUDSYNC_NETWORK_TRACE
            network_trace_log(data, method, endpoint, (long)statusCode, result.code, 0, network_trace_now_ms() - trace_start_ms);
            #endif
            return result;
        }
        
        // otherwise return a buffer
        NETWORK_RESULT result = {};
        result.code = CLOUDSYNC_NETWORK_BUFFER;
        if (zero_terminated) {
            NSString *utf8String = [[NSString alloc] initWithData:responseData encoding:NSUTF8StringEncoding];
            if (!utf8String) {
                NSString *msg = @"Response is not valid UTF-8";
                NETWORK_RESULT error_result = {CLOUDSYNC_NETWORK_ERROR, (char *)msg.UTF8String, 0, (void *)CFBridgingRetain(msg), network_buffer_cleanup};
                #ifdef CLOUDSYNC_NETWORK_TRACE
                network_trace_log(data, method, endpoint, (long)statusCode, error_result.code, 0, network_trace_now_ms() - trace_start_ms);
                #endif
                return error_result;
            }
            result.buffer = (char *)utf8String.UTF8String;
            result.xdata = (void *)CFBridgingRetain(utf8String);
        } else {
            result.buffer = (char *)responseData.bytes;
            result.xdata = (void *)CFBridgingRetain(responseData);
        }
        result.blen = [responseData length];
        result.xfree = network_buffer_cleanup;
        
        #ifdef CLOUDSYNC_NETWORK_TRACE
        network_trace_log(data, method, endpoint, (long)statusCode, result.code, result.blen, network_trace_now_ms() - trace_start_ms);
        #endif
        return result;
    }
    
    // return error
    NETWORK_RESULT result = {};
    NSString *msg = nil;
    if (responseError) {
        msg = responseError;
    } else if (responseData && [responseData length] > 0) {
        // Use the actual response body as the error message
        msg = [[NSString alloc] initWithData:responseData encoding:NSUTF8StringEncoding];
        if (!msg) {
            msg = [NSString stringWithFormat:@"HTTP %ld error", (long)statusCode];
        }
    } else {
        msg = [NSString stringWithFormat:@"HTTP %ld error", (long)statusCode];
    }
    result.code = CLOUDSYNC_NETWORK_ERROR;
    result.buffer = (char *)msg.UTF8String;
    result.xdata = (void *)CFBridgingRetain(msg);
    result.xfree = network_buffer_cleanup;
    result.blen = responseError ? (size_t)errorCode : (size_t)statusCode;
    
    #ifdef CLOUDSYNC_NETWORK_TRACE
    network_trace_log(data, method, endpoint, (long)statusCode, result.code, 0, network_trace_now_ms() - trace_start_ms);
    #endif
    return result;
}
