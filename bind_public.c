1 /*
2  * Re-map bind() on 0.0.0.0 or :: to bind() on the node's public IP address
3  * Jude Nelson (jcnelson@cs.princeton.edu)
4  */
5
6 #include <stdio.h>
7 #include <stdlib.h>
8 #include <memory.h>
9 #include <sys/types.h>
10 #include <sys/socket.h>
11 #include <limits.h>
12 #include <errno.h>
13 #include <netdb.h>
14 #include <unistd.h>
15 #include <dlfcn.h>
16
17 int (*bind_original)(int fd, struct sockaddr* addr, socklen_t len ) = NULL;
18
19 // which C library do we need to replace bind in?
20 #if defined(__LP64__) || defined(_LP64)
21 #define LIBC_PATH "/lib64/libc.so.6"
22 #else
23 #define LIBC_PATH "/lib/libc.so.6"
24 #endif
25
26 // get the node's public IP address
27 static int get_public_ip( struct sockaddr* addr ) {
28    
29    struct addrinfo hints;
30    memset( &hints, 0, sizeof(hints) );
31    hints.ai_family = addr->sa_family;
32    hints.ai_flags = AI_CANONNAME;
33    hints.ai_protocol = 0;
34    hints.ai_canonname = NULL;
35    hints.ai_addr = NULL;
36    hints.ai_next = NULL;
37    
38    int rc = 0;
39
40    // get the node hostname
41    struct addrinfo *result = NULL;
42    char hostname[HOST_NAME_MAX+1];
43    gethostname( hostname, HOST_NAME_MAX );
44
45    // get the address information from the hostname
46    rc = getaddrinfo( hostname, NULL, &hints, &result );
47    if( rc != 0 ) {
48       // could not get addr info
49       fprintf(stderr, "bind_public: get_public_ip: getaddrinfo: %s\n", gai_strerror( rc ) );
50       errno = EINVAL;
51       return -1;
52    }
53    
54    // NOTE: there should only be one IP address for this node, but it
55    // is possible that it can have more.  Here, we just take the first
56    // address given.
57    
58    switch( addr->sa_family ) {
59       case AF_INET:
60          // IPv4
61          ((struct sockaddr_in*)addr)->sin_addr = ((struct sockaddr_in*)result->ai_addr)->sin_addr;
62          break;
63          
64       case AF_INET6:
65          // IPv6
66          ((struct sockaddr_in6*)addr)->sin6_addr = ((struct sockaddr_in6*)result->ai_addr)->sin6_addr;
67          break;
68          
69       default:
70          fprintf(stderr, "bind_public: get_public_ip: unknown socket address family %d\n", addr->sa_family );
71          rc = -1;
72          break;
73    }
74    
75    freeaddrinfo( result );
76    
77    return rc;
78 }
79
80
81 // is a particular sockaddr initialized to 0.0.0.0 or ::?
82 static int is_addr_any( const struct sockaddr* addr ) {
83    int ret = 0;
84    
85    switch( addr->sa_family ) {
86       case AF_INET: {
87          // IPv4
88          struct sockaddr_in* addr4 = (struct sockaddr_in*)addr;
89          if( addr4->sin_addr.s_addr == INADDR_ANY )
90             ret = 1;    // this is 0.0.0.0
91          break;
92       }
93       case AF_INET6: {
94          // IPv6
95          struct sockaddr_in6* addr6 = (struct sockaddr_in6*)addr;
96          if( memcmp( &addr6->sin6_addr, &in6addr_any, sizeof(in6addr_any) ) == 0 )
97             ret = 1;    // this is ::
98          break;
99       }
100       default:
101          // unsupported bind
102          fprintf(stderr, "bind_public: is_addr_any: unsupported socket address family %d\n", addr->sa_family );
103          ret = -1;
104          break;
105    }
106    
107    return ret;
108 }
109
110
111 // copy over non-IP-related fields from one address to another
112 static int copy_nonIP_fields( struct sockaddr* dest, const struct sockaddr* src ) {
113    int rc = 0;
114    
115    switch( src->sa_family ) {
116       case AF_INET: {
117          // IPv4
118          struct sockaddr_in* dest4 = (struct sockaddr_in*)dest;
119          struct sockaddr_in* src4 = (struct sockaddr_in*)src;
120          
121          dest4->sin_family = src4->sin_family;
122          dest4->sin_port = src4->sin_port;
123          break;
124       }
125       case AF_INET6: {
126          // IPv6
127          struct sockaddr_in6* dest6 = (struct sockaddr_in6*)dest;
128          struct sockaddr_in6* src6 = (struct sockaddr_in6*)src;
129          
130          dest6->sin6_family = src6->sin6_family;
131          dest6->sin6_port = src6->sin6_port;
132          dest6->sin6_flowinfo = src6->sin6_flowinfo;
133          dest6->sin6_scope_id = src6->sin6_scope_id;
134          break;
135       }
136       default:
137          // unsupported bind
138          fprintf(stderr, "bind_public: copy_nonIP_fields: unsupported socket address family %d\n", src->sa_family );
139          rc = -1;
140          break;
141    }
142    
143    return rc;
144 }
145
146
147 static void print_ip4( uint32_t i ) {
148    i = htonl( i );
149    printf("%i.%i.%i.%i",
150           (i >> 24) & 0xFF,
151           (i >> 16) & 0xFF,
152           (i >> 8) & 0xFF,
153           i & 0xFF);
154 }
155
156 static void print_ip6( uint8_t* bytes ) {
157    printf("%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x:%x",
158           bytes[15], bytes[14], bytes[13], bytes[12],
159           bytes[11], bytes[10], bytes[9],  bytes[8],
160           bytes[7],  bytes[6],  bytes[5],  bytes[4],
161           bytes[3],  bytes[2],  bytes[1],  bytes[0] );
162 }
163
164 static void debug( const struct sockaddr* before, struct sockaddr* after ) {
165    printf("bind_public: ");
166    switch( before->sa_family ) {
167       case AF_INET:
168          print_ip4( ((struct sockaddr_in*)before)->sin_addr.s_addr );
169          printf(" --> ");
170          print_ip4( ((struct sockaddr_in*)after)->sin_addr.s_addr );
171          printf("\n");
172          break;
173       case AF_INET6:
174          print_ip6( ((struct sockaddr_in6*)before)->sin6_addr.s6_addr );
175          printf(" --> " );
176          print_ip6( ((struct sockaddr_in6*)after)->sin6_addr.s6_addr );
177          printf("\n");
178          break;
179       default:
180          printf("UNKNOWN --> UNKNOWN\n");
181          break;
182    }
183    fflush( stdout );
184 }
185
186 // if the caller attempted to bind to 0.0.0.0 or ::, then change it to
187 // this node's public IP address
188 int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
189
190    // save the original bind() call
191    void *handle = dlopen( LIBC_PATH, RTLD_LAZY );
192    if (!handle) {
193       fprintf( stderr, "Error loading libc.so.6\n" );
194       fflush( stderr );
195       return -1;
196    }
197    bind_original = dlsym(handle, "bind");
198    if( bind_original == NULL ) {
199       fprintf( stderr, "Error loading socket symbol\n" );
200       fflush( stderr );
201       return -1;
202    }
203
204    int rc = is_addr_any( addr );
205    if( rc > 0 ) {
206
207       // rewrite this address
208       struct sockaddr_storage new_addr;
209       memset( &new_addr, 0, sizeof(struct sockaddr_storage));
210
211       if( copy_nonIP_fields( (struct sockaddr*)&new_addr, addr ) != 0 ) {
212          errno = EACCES;
213          rc = -1;
214       }
215       else if( get_public_ip( (struct sockaddr*)&new_addr ) != 0 ) {
216          rc = -1;
217       }
218       else {
219          // Un-comment the following line to activate the debug message
220          //debug( addr, (struct sockaddr*)&new_addr );
221          rc = bind_original( sockfd, (struct sockaddr*)&new_addr, addrlen );
222       }
223    }
224    else {
225       return bind_original( sockfd, addr, addrlen );
226    }
227 }
