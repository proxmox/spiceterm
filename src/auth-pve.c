#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "spiceterm.h"

static char *auth_path = "/";
static char *auth_perm = "Sys.Console";

void pve_auth_set_path(char *path) { auth_path = path; }

void pve_auth_set_permissions(char *perm) { auth_perm = perm; }

static char *urlencode(char *buf, const char *value) {
    static const char *hexchar = "0123456789abcdef";
    char *p = buf;
    int i;
    int l = strlen(value);
    for (i = 0; i < l; i++) {
        char c = value[i];
        // clang-format off
        if (('a' <= c && c <= 'z') ||
            ('A' <= c && c <= 'Z') ||
            ('0' <= c && c <= '9')) {
            *p++ = c;
        } else if (c == 32) {
            *p++ = '+';
        } else {
            *p++ = '%';
            *p++ = hexchar[c >> 4];
            *p++ = hexchar[c & 15];
        }
        // clang-format on
    }
    *p = 0;

    return p;
}

int pve_auth_verify(const char *clientip, const char *username, const char *passwd) {
    struct sockaddr_in server;

    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd == -1) {
        perror("pve_auth_verify: socket failed");
        return -1;
    }

    struct hostent *he;
    if ((he = gethostbyname("localhost")) == NULL) {
        fprintf(stderr, "pve_auth_verify: error resolving hostname\n");
        goto err;
    }

    memcpy(&server.sin_addr, he->h_addr_list[0], he->h_length);
    server.sin_family = AF_INET;
    server.sin_port = htons(85);

    if (connect(sfd, (struct sockaddr *)&server, sizeof(server))) {
        perror("pve_auth_verify: error connecting to server");
        goto err;
    }

    char buf[8292];
    char form[8092];

    char *p = form;
    p = urlencode(p, "username");
    *p++ = '=';
    p = urlencode(p, username);

    *p++ = '&';
    p = urlencode(p, "password");
    *p++ = '=';
    p = urlencode(p, passwd);

    *p++ = '&';
    p = urlencode(p, "path");
    *p++ = '=';
    p = urlencode(p, auth_path);

    *p++ = '&';
    p = urlencode(p, "privs");
    *p++ = '=';
    p = urlencode(p, auth_perm);

    sprintf(
        buf,
        "POST /api2/json/access/ticket HTTP/1.1\n"
        "Host: localhost:85\n"
        "Connection: close\n"
        "PVEClientIP: %s\n"
        "Content-Type: application/x-www-form-urlencoded\n"
        "Content-Length: %zd\n\n%s\n",
        clientip, strlen(form), form
    );
    ssize_t len = strlen(buf);
    ssize_t sb = send(sfd, buf, len, 0);
    if (sb < 0) {
        perror("pve_auth_verify: send failed");
        goto err;
    }
    if (sb != len) {
        fprintf(stderr, "pve_auth_verify: partial send error\n");
        goto err;
    }

    len = recv(sfd, buf, sizeof(buf) - 1, 0);
    if (len < 0) {
        perror("pve_auth_verify: recv failed");
        goto err;
    }

    buf[len] = 0;

    // printf("DATA:%s\n", buf);

    shutdown(sfd, SHUT_RDWR);

    if (!strncmp(buf, "HTTP/1.1 200 OK", 15)) {
        return 0;
    }

    char *firstline = strtok(buf, "\n");

    fprintf(stderr, "auth failed: %s\n", firstline);

    return -1;

err:
    shutdown(sfd, SHUT_RDWR);
    return -1;
}
