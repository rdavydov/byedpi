#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <limits.h>

#include <params.h>
#include <proxy.h>
#include <packets.h>
#include <error.h>

#ifndef _WIN32
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <netdb.h>
    #include <fcntl.h>
    #include <netinet/tcp.h>
#else
    #include <ws2tcpip.h>
    #define close(fd) closesocket(fd)
#endif

#define VERSION 8
#define MPOOL_INC 16

char oob_char[1] = "a";

char ip_option[1] = "\0";

struct packet fake_tls = { 
    sizeof(tls_data), tls_data 
},
fake_http = { 
    sizeof(http_data), http_data
},
oob_data = { 
    sizeof(oob_char), oob_char
};


struct params params = {
    .sfdelay = 3,
    .wait_send = 1,
    
    .cache_ttl = 100800,
    .ipv6 = 1,
    .resolve = 1,
    .max_open = 512,
    .bfsize = 16384,
    .baddr = {
        .sin6_family = AF_INET6
    },
    .debug = 0
};


const char help_text[] = {
    "    -i, --ip, <ip>            Listening IP, default 0.0.0.0\n"
    "    -p, --port <num>          Listening port, default 1080\n"
    "    -c, --max-conn <count>    Connection count limit, default 512\n"
    "    -N, --no-domain           Deny domain resolving\n"
    "    -I  --conn-ip <ip>        Connection binded IP, default ::\n"
    "    -b, --buf-size <size>     Buffer size, default 16384\n"
    "    -x, --debug <level>       Print logs, 0, 1 or 2\n"
    "    -g, --def-ttl <num>       TTL for all outgoing connections\n"
    // desync options
    "    -K, --desync-known        Desync only HTTP and TLS with SNI\n"
    #ifdef TCP_FASTOPEN_CONNECT
    "    -F, --tfo                 Enable TCP Fast Open\n"
    #endif
    "    -A, --auto[=t,r,c,s,a]    Try desync params after this option\n"
    "                              Detect: torst,redirect,cl_err,sid_inv,alert\n"
    "    -u, --cache-ttl <sec>     Lifetime of cached desync params for IP\n"
    #ifdef TIMEOUT_SUPPORT
    "    -T, --timeout <sec>       Timeout waiting for response, after which trigger auto\n"
    #endif
    "    -s, --split <n[+s]>       Split packet at n\n"
    "                              +s - add SNI offset\n"
    "                              +h - add HTTP Host offset\n"
    "    -d, --disorder <n[+s]>    Split and send reverse order\n"
    "    -o, --oob <n[+s]>         Split and send as OOB data\n"
    #ifdef FAKE_SUPPORT
    "    -f, --fake <n[+s]>        Split and send fake packet\n"
    "    -t, --ttl <num>           TTL of fake packets, default 8\n"
    "    -k, --ip-opt [f|:str]     IP options of fake packets\n"
    #ifdef __linux__
    "    -S, --md5sig              Add MD5 Signature option for fake packets\n"
    #endif
    "    -l, --fake-tls <f|:str>\n"
    "    -j, --fake-http <f|:str>  Set custom fake packet\n"
    "    -n, --tls-sni <str>       Change SNI in fake ClientHello\n"
    #endif
    "    -e, --oob-data <f|:str>   Set custom OOB data, filename or :string\n"
    "    -M, --mod-http <h,d,r>    Modify HTTP: hcsmix,dcsmix,rmspace\n"
    "    -r, --tlsrec <n[+s]>      Make TLS record at position\n"
};


const struct option options[] = {
    {"no-domain",     0, 0, 'N'},
    {"no-ipv6",       0, 0, 'X'},
    {"help",          0, 0, 'h'},
    {"version",       0, 0, 'v'},
    {"ip",            1, 0, 'i'},
    {"port",          1, 0, 'p'},
    {"conn-ip",       1, 0, 'I'},
    {"buf-size",      1, 0, 'b'},
    {"max-conn",      1, 0, 'c'},
    {"debug",         1, 0, 'x'},
    
    {"desync-known ", 0, 0, 'K'},
    #ifdef TCP_FASTOPEN_CONNECT
    {"tfo ",          0, 0, 'F'},
    #endif
    {"auto",          2, 0, 'A'},
    {"cache-ttl",     1, 0, 'u'},
    #ifdef TIMEOUT_SUPPORT
    {"timeout",       1, 0, 'T'},
    #endif
    {"split",         1, 0, 's'},
    {"disorder",      1, 0, 'd'},
    {"oob",           1, 0, 'o'},
    #ifdef FAKE_SUPPORT
    {"fake",          1, 0, 'f'},
    {"ttl",           1, 0, 't'},
    {"ip-opt",        2, 0, 'k'},
    #ifdef __linux__
    {"md5sig",        0, 0, 'S'},
    #endif
    {"fake-tls",      1, 0, 'l'},
    {"fake-http",     1, 0, 'j'},
    {"tls-sni",       1, 0, 'n'},
    #endif
    {"oob-data",      1, 0, 'e'},
    {"mod-http",      1, 0, 'M'},
    {"tlsrec",        1, 0, 'r'},
    {"def-ttl",       1, 0, 'g'},
    {"delay",         1, 0, 'w'}, //
    {"not-wait-send", 0, 0, 'W'}, //
    {0}
};
    

char *parse_cform(const char *str, ssize_t *size)
{
    ssize_t len = strlen(str);
    char *d = malloc(len);
    if (!d) {
        return 0;
    }
    static char esca[] = {
        'r','\r','n','\n','t','\t','\\','\\',
        'f','\f','b','\b','v','\v','a','\a', 0
    };
    ssize_t i = 0, p = 0;
    for (; p < len; ++p && ++i) {
        if (str[p] != '\\') {
            d[i] = str[p];
            continue;
        }
        p++;
        char *e = esca;
        for (; *e; e += 2) {
            if (*e == str[p]) {
                d[i] = *(e + 1);
                break;
            }
        }
        if (*e) {
            continue;
        }
        int n = 0;
        if (sscanf(&str[p], "x%2hhx%n", &d[i], &n) == 1
              || sscanf(&str[p], "%3hho%n", &d[i], &n) == 1) {
            p += (n - 1);
            continue;
        }
        i--; p--;
    }
    *size = i;
    char *m = realloc(d, i);
    return m ? m : d;
}


char *ftob(const char *str, ssize_t *sl)
{
    if (*str == ':') {
        return parse_cform(str + 1, sl);
    }
    char *buffer = 0;
    long size;
    
    FILE *file = fopen(str, "rb");
    if (!file)
        return 0;
    do {
        if (fseek(file, 0, SEEK_END)) {
            break;
        }
        size = ftell(file);
        if (!size || fseek(file, 0, SEEK_SET)) {
            break;
        }
        if (!(buffer = malloc(size))) {
            break;
        }
        if (fread(buffer, 1, size, file) != size) {
            free(buffer);
            buffer = 0;
        }
    } while (0);
    if (buffer) {
        *sl = size;
    }
    fclose(file);
    return buffer;
}


int get_addr(const char *str, struct sockaddr_ina *addr)
{
    struct addrinfo hints = {0}, *res = 0;
    
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags = AI_NUMERICHOST;
    
    if (getaddrinfo(str, 0, &hints, &res) || !res) {
        return -1;
    }
    if (res->ai_addr->sa_family == AF_INET6)
        addr->in6 = *(struct sockaddr_in6 *)res->ai_addr;
    else
        addr->in = *(struct sockaddr_in *)res->ai_addr;
    freeaddrinfo(res);
    
    return 0;
}


int get_default_ttl()
{
    int orig_ttl = -1, fd;
    socklen_t tsize = sizeof(orig_ttl);
    
    if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        uniperror("socket");
        return -1;
    }
    if (getsockopt(fd, IPPROTO_IP, IP_TTL,
             (char *)&orig_ttl, &tsize) < 0) {
        uniperror("getsockopt IP_TTL");
    }
    close(fd);
    return orig_ttl;
}


int parse_offset(struct part *part, const char *str)
{
    char *end = 0;
    long val = strtol(str, &end, 0);
    if (*end == '+') switch (*(end + 1)) {
        case 's': 
            part->flag = OFFSET_SNI;
            break;
        case 'h': 
            part->flag = OFFSET_HOST;
            break;
        default:
            return -1;
    }
    else if (*end) {
        return -1;
    }
    part->pos = val;
    return 0;
}


void *add(void **root, int *n, size_t ss)
{
    void *p = realloc(*root, ss * (*n + 1));
    if (!p) {
        uniperror("realloc");
        return 0;
    }
    *root = p;
    p = ((*root) + ((*n) * ss));
    memset(p, 0, ss);
    *n = *n + 1;
    return p;
}


void clear_params(void)
{
    #ifdef _WIN32
    WSACleanup();
    #endif
    if (params.mempool) {
        mem_destroy(params.mempool);
        params.mempool = 0;
    }
    if (params.dp) {
        for (int i = 0; i < params.dp_count; i++) {
            struct desync_params s = params.dp[i];
            if (s.ip_options != ip_option) {
                free(s.ip_options);
                s.ip_options = ip_option;
            }
            if (s.parts != 0) {
                free(s.parts);
                s.parts = 0;
            }
        }
        free(params.dp);
        params.dp = 0;
    }
    if (fake_tls.data != tls_data) {
        free(fake_tls.data);
        fake_tls.data = tls_data;
    }
    if (fake_http.data != http_data) {
        free(fake_http.data);
        fake_http.data = http_data;
    }
    if (oob_data.data != oob_char) {
        free(oob_data.data);
        oob_data.data = oob_char;
    }
}


int main(int argc, char **argv) 
{
    #ifdef _WIN32
    WSADATA wsa;
    
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) {
        uniperror("WSAStartup");
        return -1;
    }
    #endif
    struct sockaddr_ina s = {
        .in = {
            .sin_family = AF_INET,
            .sin_port = htons(1080)
    }},
    b = { .in6 = params.baddr };
    
    int optc = sizeof(options)/sizeof(*options);
    for (int i = 0, e = optc; i < e; i++)
        optc += options[i].has_arg;
        
    char opt[optc + 1];
    opt[optc] = 0;
    
    for (int i = 0, o = 0; o < optc; i++, o++) {
        opt[o] = options[i].val;
        for (int c = options[i].has_arg; c; c--) {
            o++;
            opt[o] = ':';
        }
    }
    
    int rez;
    int invalid = 0;
    
    long val = 0;
    char *end = 0;
    
    uint16_t port = htons(1080);
    
    struct desync_params *dp = add((void *)&params.dp,
        &params.dp_count, sizeof(struct desync_params));
    if (!dp) {
        clear_params();
        return -1;
    }
    while (!invalid && (rez = getopt_long_only(
             argc, argv, opt, options, 0)) != -1) {
        switch (rez) {
        
        case 'N':
            params.resolve = 0;
            break;
        case 'X':
            params.ipv6 = 0;
            break;
        case 'h':
            printf(help_text);
            clear_params();
            return 0;
        case 'v':
            printf("%d\n", VERSION);
            clear_params();
            return 0;
        
        case 'i':
            if (get_addr(optarg, &s) < 0)
                invalid = 1;
            break;
            
        case 'p':
            val = strtol(optarg, &end, 0);
            if (val <= 0 || val > 0xffff || *end)
                invalid = 1;
            else
                port = htons(val);
            break;
            
        case 'I':
            if (get_addr(optarg, &b) < 0)
                invalid = 1;
            else
                params.baddr = b.in6;
            break;
            
        case 'b':
            val = strtol(optarg, &end, 0);
            if (val <= 0 || val > INT_MAX/4 || *end)
                invalid = 1;
            else
                params.bfsize = val;
            break;
            
        case 'c':
            val = strtol(optarg, &end, 0);
            if (val <= 0 || val >= (0xffff/2) || *end) 
                invalid = 1;
            else
                params.max_open = val;
            break;
           
        case 'x': //
            params.debug = strtol(optarg, 0, 0);
            if (params.debug < 0)
                invalid = 1;
            break;
            
        // desync options
        
        case 'K':
            params.de_known = 1;
            break;
            
        case 'F':
            params.tfo = 1;
            break;
            
        case 'A':
            dp = add((void *)&params.dp, &params.dp_count,
                sizeof(struct desync_params));
            if (!dp) {
                clear_params();
                return -1;
            }
            if (!optarg) {
                dp->detect |= DETECT_TORST;
                break;
            }
            end = optarg;
            while (end && !invalid) {
                switch (*end) {
                    case 't': 
                        dp->detect |= DETECT_TORST;
                        break;
                    case 'r': 
                        dp->detect |= DETECT_HTTP_LOCAT;
                        break;
                    case 'c': 
                        dp->detect |= DETECT_HTTP_CLERR;
                        break;
                    case 's': 
                        dp->detect |= DETECT_TLS_INVSID;
                        break;
                    case 'a': 
                        dp->detect |= DETECT_TLS_ALERT;
                        break;
                    default:
                        invalid = 1;
                        continue;
                }
                end = strchr(end, ',');
                if (end) end++;
            }
            break;
            
        case 'u':
            val = strtol(optarg, &end, 0);
            if (val <= 0 || *end) 
                invalid = 1;
            else
                params.cache_ttl = val;
            break;
        
        case 'T':;
            #ifdef __linux__
            float f = strtof(optarg, &end);
            val = (long)(f * 1000);
            #else
            val = strtol(optarg, &end, 0);
            #endif
            if (val <= 0 || val > UINT_MAX || *end)
                invalid = 1;
            else
                params.timeout = val;
            break;
            
        case 's':
        case 'd':
        case 'o':
        case 'f':
            ;
            struct part *part = add((void *)&dp->parts,
                &dp->parts_n, sizeof(struct part));
            if (!part) {
                clear_params();
                return -1;
            }
            if (parse_offset(part, optarg)) {
                invalid = 1;
                break;
            }
            switch (rez) {
                case 's': part->m = DESYNC_SPLIT;
                    break;
                case 'd': part->m = DESYNC_DISORDER;
                    break;
                case 'o': part->m = DESYNC_OOB;
                    break;
                case 'f': part->m = DESYNC_FAKE;
            }
            break;
            
        case 't':
            val = strtol(optarg, &end, 0);
            if (val <= 0 || val > 255 || *end) 
                invalid = 1;
            else
                dp->ttl = val;
            break;
            
        case 'k':
            if (optarg)
                dp->ip_options = ftob(optarg, &dp->ip_options_len);
            else {
                dp->ip_options = ip_option;
                dp->ip_options_len = sizeof(ip_option);
            }
            if (!dp->ip_options) {
                uniperror("read/parse");
                invalid = 1;
            }
            break;
            
        case 'S':
            dp->md5sig = 1;
            break;
            
        case 'n':
            if (change_tls_sni(optarg, fake_tls.data, fake_tls.size)) {
                fprintf(stderr, "error chsni\n");
                clear_params();
                return -1;
            }
            printf("sni: %s\n", optarg);
            break;
            
        case 'l':
            fake_tls.data = ftob(optarg, &fake_tls.size);
            if (!fake_tls.data) {
                uniperror("read/parse");
                invalid = 1;
            }
            break;
            
        case 'j':
            fake_http.data = ftob(optarg, &fake_http.size);
            if (!fake_http.data) {
                uniperror("read/parse");
                invalid = 1;
            }
            break;
            
        case 'e':
            oob_data.data = ftob(optarg, &oob_data.size);
            if (!oob_data.data) {
                uniperror("read/parse");
                invalid = 1;
            }
            break;
            
        case 'M':
            end = optarg;
            while (end && !invalid) {
                switch (*end) {
                    case 'r': 
                        dp->mod_http |= MH_SPACE;
                        break;
                    case 'h': 
                        dp->mod_http |= MH_HMIX;
                        break;
                    case 'd': 
                        dp->mod_http |= MH_DMIX;
                        break;
                    default:
                        invalid = 1;
                        continue;
                }
                end = strchr(end, ',');
                if (end) end++;
            }
            break;
            
        case 'r':
            part = add((void *)&dp->tlsrec,
                &dp->tlsrec_n, sizeof(struct part));
            if (!part) {
                clear_params();
                return -1;
            }
            if (parse_offset(part, optarg)
                   || part->pos > 0xffff) {
                invalid = 1;
                break;
            }
            break;
            
        case 'g':
            val = strtol(optarg, &end, 0);
            if (val <= 0 || val > 255 || *end)
                invalid = 1;
            else {
                params.def_ttl = val;
                params.custom_ttl = 1;
            }
            break;
            
        case 'w': //
            params.sfdelay = strtol(optarg, &end, 0);
            if (params.sfdelay < 0 || optarg == end 
                    || params.sfdelay >= 1000 || *end)
                invalid = 1;
            break;
        
        case 'W':
            params.wait_send = 0;
            break;
            
        case 0:
            break;
            
        case '?':
            clear_params();
            return -1;
            
        default: 
            printf("?: %c\n", rez);
            clear_params();
            return -1;
        }
    }
    if (invalid) {
        fprintf(stderr, "invalid value: -%c %s\n", rez, optarg);
        clear_params();
        return -1;
    }
    s.in.sin_port = port;
    b.in.sin_port = 0;
    
    if (b.sa.sa_family != AF_INET6) {
        params.ipv6 = 0;
    }
    if (!params.def_ttl) {
        if ((params.def_ttl = get_default_ttl()) < 1) {
            clear_params();
            return -1;
        }
    }
    params.mempool = mem_pool(MPOOL_INC);
    if (!params.mempool) {
        uniperror("mem_pool");
        clear_params();
        return -1;
    }
    int status = run(&s);
    clear_params();
    return status;
}
