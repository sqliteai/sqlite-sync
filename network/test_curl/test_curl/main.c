//
//  main.c
//  test_curl
//
//  Created by Marco Bambini on 06/12/24.
//

#include <stdio.h>
#include "curl/curl.h"

// POST:     https://curl.se/libcurl/c/http-post.html
// DOWNLOAD: https://curl.se/libcurl/c/url2file.html and https://stackoverflow.com/questions/1636333/download-file-using-libcurl-in-c-c
// UPLOAD:   https://curl.se/libcurl/c/fileupload.html

void test_url (void) {
    const char *connection_string = "https://czxfah3riz.sqlite.cloud:8860/chinook.sqlite?apikey=tYDanFolWR6AQF0l46dorXqYhjwPOztMw6tb6fbPmpo";
    
    CURLU *url = curl_url();
    CURLUcode rc = curl_url_set(url, CURLUPART_URL, connection_string, 0);
    if (rc == CURLE_OK) {
        char *scheme = NULL;;
        rc = curl_url_get(url, CURLUPART_SCHEME, &scheme, 0);
        if (scheme) printf("CURLUPART_SCHEME: %s\n", scheme);
        
        char *url2 = NULL;;
        rc = curl_url_get(url, CURLUPART_URL, &url2, 0);
        if (url2) printf("CURLUPART_URL: %s\n", url2);
        
        char *user = NULL;;
        rc = curl_url_get(url, CURLUPART_USER, &user, 0);
        if (user) printf("CURLUPART_USER: %s\n", user);
        
        char *password = NULL;;
        rc = curl_url_get(url, CURLUPART_PASSWORD, &password, 0);
        if (password) printf("CURLUPART_PASSWORD: %s\n", password);
        
        char *options = NULL;;
        rc = curl_url_get(url, CURLUPART_OPTIONS, &options, 0);
        if (options) printf("CURLUPART_OPTIONS: %s\n", options);
        
        char *host = NULL;;
        rc = curl_url_get(url, CURLUPART_HOST, &host, 0);
        if (host) printf("CURLUPART_HOST: %s\n", host);
        
        char *port = NULL;;
        rc = curl_url_get(url, CURLUPART_PORT, &port, 0);
        if (port) printf("CURLUPART_PORT: %s\n", port);
        
        char *path = NULL;;
        rc = curl_url_get(url, CURLUPART_PATH, &path, 0);
        if (path) printf("CURLUPART_PATH: %s\n", path);
        
        char *query = NULL;;
        rc = curl_url_get(url, CURLUPART_QUERY, &query, 0);
        if (query) printf("CURLUPART_QUERY: %s\n", query);
        
        char *fragment = NULL;
        rc = curl_url_get(url, CURLUPART_FRAGMENT, &fragment, 0);
        if (fragment) printf("CURLUPART_FRAGMENT: %s\n", fragment);
    }
    
    if (url) curl_url_cleanup(url);
}

void test_sql (void) {
    CURL *curl = curl_easy_init();
    
    if(curl) {
        curl_easy_setopt(curl, CURLOPT_URL, "https://czxfah3riz.sqlite.cloud:8090/v2/weblite/sql");
        
        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, "Authorization: Bearer sqlitecloud://czxfah3riz.sqlite.cloud:8860/chinook.sqlite?apikey=tYDanFolWR6AQF0l46dorXqYhjwPOztMw6tb6fbPmpo");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "{\"sql\":\"SELECT * FROM albums LIMIT 18;\", \"database\": \"chinook.sqlite\"}");
        
        
        CURLcode res = curl_easy_perform(curl);
        
        if (res != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        } else {
            printf("OK\n");
        }
        
        curl_easy_cleanup(curl);
    }
}

int main (void) {
    curl_global_init(CURL_GLOBAL_ALL);
    test_url();
    test_sql();
    curl_global_cleanup();
    return 0;
}
