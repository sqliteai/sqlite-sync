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

static NSString *network_string_from_cstr (const char *value) {
    return value ? [NSString stringWithUTF8String:value] : nil;
}

static NETWORK_RESULT network_error_result (NSString *message, size_t blen) {
    NETWORK_RESULT result = {};
    result.code = CLOUDSYNC_NETWORK_ERROR;
    result.buffer = message ? (char *)message.UTF8String : NULL;
    result.xdata = message ? (void *)CFBridgingRetain(message) : NULL;
    result.xfree = network_buffer_cleanup;
    result.blen = blen;
    return result;
}

static void network_set_header (NSMutableURLRequest *request, const char *header) {
    NSString *headerString = network_string_from_cstr(header);
    if (!headerString) return;

    NSRange separator = [headerString rangeOfString:@":"];
    if (separator.location == NSNotFound) return;

    NSCharacterSet *ws = [NSCharacterSet whitespaceCharacterSet];
    NSString *field = [[headerString substringToIndex:separator.location] stringByTrimmingCharactersInSet:ws];
    NSString *value = [[headerString substringFromIndex:separator.location + 1] stringByTrimmingCharactersInSet:ws];
    if (field.length == 0) return;
    [request setValue:value forHTTPHeaderField:field];
}

bool network_send_buffer(network_data *data, const char *endpoint, const char *authentication, const void *blob, int blob_size) {
    @autoreleasepool {
        if (!endpoint || blob_size < 0 || (!blob && blob_size > 0)) return false;

        NSString *urlString = network_string_from_cstr(endpoint);
        NSURL *url = urlString ? [NSURL URLWithString:urlString] : nil;
        if (!url) return false;

        NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
        [request setHTTPMethod:@"PUT"];
        [request setValue:@"application/octet-stream" forHTTPHeaderField:@"Content-Type"];
        [request setValue:@"text/plain" forHTTPHeaderField:@"Accept"];

        if (authentication && authentication[0] != '\0') {
            NSString *authValue = network_string_from_cstr(authentication);
            if (authValue) {
                [request setValue:[NSString stringWithFormat:@"Bearer %@", authValue] forHTTPHeaderField:@"Authorization"];
            }
        }

        char *org_id = data ? network_data_get_orgid(data) : NULL;
        NSString *orgValue = network_string_from_cstr(org_id);
        if (orgValue) {
            [request setValue:orgValue forHTTPHeaderField:@CLOUDSYNC_HEADER_ORG];
        }

        NSData *bodyData = blob_size > 0 ? [NSData dataWithBytes:blob length:(NSUInteger)blob_size] : [NSData data];
        [request setHTTPBody:bodyData];

        __block bool success = false;
        dispatch_semaphore_t sema = dispatch_semaphore_create(0);

        NSURLSessionConfiguration *config = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        NSURLSession *session = [NSURLSession sessionWithConfiguration:config];

        NSURLSessionDataTask *task = [session dataTaskWithRequest:request
                                                completionHandler:^(NSData * _Nullable responseBody,
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
        [session finishTasksAndInvalidate];

        return success;
    }
}


NETWORK_RESULT network_receive_buffer(network_data *data, const char *endpoint, const char *authentication, bool zero_terminated, bool is_post_request, char *json_payload, const char *custom_header) {
    @autoreleasepool {
        NSString *urlString = network_string_from_cstr(endpoint);
        NSURL *url = urlString ? [NSURL URLWithString:urlString] : nil;
        if (!url) return network_error_result(@"Malformed URL", 0);

        NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
        request.HTTPMethod = (json_payload || is_post_request) ? @"POST" : @"GET";

        network_set_header(request, custom_header);

        char *org_id = data ? network_data_get_orgid(data) : NULL;
        NSString *orgValue = network_string_from_cstr(org_id);
        if (orgValue) {
            [request setValue:orgValue forHTTPHeaderField:@CLOUDSYNC_HEADER_ORG];
        }

        if (authentication && authentication[0] != '\0') {
            NSString *authValue = network_string_from_cstr(authentication);
            if (authValue) {
                [request setValue:[NSString stringWithFormat:@"Bearer %@", authValue] forHTTPHeaderField:@"Authorization"];
            }
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
                return (NETWORK_RESULT){CLOUDSYNC_NETWORK_OK, NULL, 0, NULL, NULL};
            }

            // otherwise return a buffer
            NETWORK_RESULT result = {};
            result.code = CLOUDSYNC_NETWORK_BUFFER;
            if (zero_terminated) {
                NSString *utf8String = [[NSString alloc] initWithData:responseData encoding:NSUTF8StringEncoding];
                if (!utf8String) return network_error_result(@"Response is not valid UTF-8", 0);
                result.buffer = (char *)utf8String.UTF8String;
                result.xdata = (void *)CFBridgingRetain(utf8String);
            } else {
                result.buffer = (char *)responseData.bytes;
                result.xdata = (void *)CFBridgingRetain(responseData);
            }
            result.blen = [responseData length];
            result.xfree = network_buffer_cleanup;

            return result;
        }

        // return error
        NSString *msg = nil;
        size_t blen = responseError ? (size_t)errorCode : (size_t)statusCode;
        if (responseError) {
            msg = responseError;
        } else if (responseData && [responseData length] > 0) {
            // Use the actual response body as the error message
            msg = [[NSString alloc] initWithData:responseData encoding:NSUTF8StringEncoding];
            if (msg) blen = [responseData length];
            else msg = [NSString stringWithFormat:@"HTTP %ld error", (long)statusCode];
        }

        return network_error_result(msg, blen);
    }
}
