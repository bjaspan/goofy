/*
 * Goofy: A slightly different web load testing tool that simulates
 * waves of surfers hitting a site. Goofy initiates a fixed number of
 * connections to a URL every specified time period, letting them all
 * run in parallel until they finish. Each time period, it reports on
 * the number of connections opened, closed, and open, as well as how
 * connections closed (syscall error or HTTP status).
 */

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>
#include <sys/resource.h>

#include <map>
#include <iostream>
#include <vector>

#include "url.hh"

typedef std::map<int,int> intmap;
typedef std::map<int, const char *> strmap;
typedef std::vector<std::string> strvec;
typedef std::vector<url> urlvec;
typedef std::vector<struct sockaddr_in> addrvec;

struct wave_stat {
    wave_stat() {
	clear();
    }
    void clear() {
	opened = closed = connected = 0;
	socket.clear();
	connect.clear();
	write.clear();
	http_code.clear();
    }
    int opened;
    int connected;
    int closed;
    intmap socket;
    intmap connect;
    intmap read;
    intmap write;
    intmap http_code;
};

class time_interval {
public:
    time_interval(const char *_label) : label(_label) {
	set(0);
	mark();
    }

    // Set this.marked to the current time.
    void mark() {
	gettod(&marked);
    }

    // Set this.marked to the given timeval.
    void mark(struct timeval *t) {
	memcpy(&marked, t, sizeof(marked));
    }

    // Set and get the interval used by passed().
    void set(int _interval) { interval = _interval; }
    int get() const { return interval; }

    // Return TRUE if now-this.marked exceeds the interval.
    int passed(struct timeval *now) const {
	struct timeval delta;
	timeval_subtract(&delta, now, &marked);
	//printf("%s: %d, %d, %d, %d, %s\n", label, interval, delta.tv_sec, delta.tv_usec, delta.tv_sec*1000000+delta.tv_usec, delta.tv_sec*1000000+delta.tv_usec > interval ? "true" : "false" );
	return  ((delta.tv_sec*1000000+delta.tv_usec) > interval);
    }

    // Fill in out with time elapsed since last mark.
    void since(struct timeval *out) const {
	struct timeval now;
	gettod(&now);
	timeval_subtract(out, &now, &marked);
    }

    // STATIC. Fill in now with the current time of day.
    static void gettod(struct timeval *now) {
	if (gettimeofday(now, NULL) < 0) {
	    perror("gettimeofday");
	    exit(1);
	}
    }

    // STATIC. Fill in RESULT with X-Y.
    // Return 1 if the difference is negative, otherwise 0.
    static int timeval_subtract (struct timeval *result, const struct timeval *x, const struct timeval *y)
    {
	struct timeval _y(*y);

	/* Perform the carry for the later subtraction by updating y. */
	if (x->tv_usec < _y.tv_usec) {
	    int nsec = (_y.tv_usec - x->tv_usec) / 1000000 + 1;
	    _y.tv_usec -= 1000000 * nsec;
	    _y.tv_sec += nsec;
	}
	if (x->tv_usec - _y.tv_usec > 1000000) {
	    int nsec = (x->tv_usec - _y.tv_usec) / 1000000;
	    _y.tv_usec += 1000000 * nsec;
	    _y.tv_sec -= nsec;
	}

	/* Compute the time remaining to wait.
	   tv_usec is certainly positive. */
	result->tv_sec = x->tv_sec - _y.tv_sec;
	result->tv_usec = x->tv_usec - _y.tv_usec;

	/* Return 1 if result is negative. */
	return x->tv_sec < _y.tv_sec;
    }

    struct timeval marked, now;
    int interval;
    const char *label;
};

enum conn_state { CONN_UNUSED = 0, CONN_CONNECTING, CONN_ESTABLISHED, };
struct conn_info_t {
    int request_number;
    int url_number;
    enum conn_state state;
    struct timeval connecting;
    struct timeval connected;
};

struct wave_stat wave_stats;
struct conn_info_t *conn_info;
int request_count;
int debug;
struct pollfd *fds;
int fds_len = 0;
strmap http_codes;

void usage() {
    fprintf(stderr, "Usage: goofy [args] url [url...]\n"
            "  -n num           number of requests per wave\n"
            "  -t ms[:limit]    milliseconds between waves; limit total waves\n"
            "                   default is one wave\n"
            "  -r ms            milliseconds between reports; defaults to -t or 1000\n"
            "  -m secs          total seconds to run test; default is unlimited\n"
            "  -f fds           maximum number of sockets to request from the os\n"
            "  -h hdr           add hdr (\"Header: value\") to each request\n"
            "  -d               debug\n");
    exit(1);
}

/* Set fd to be non-blocking. */
void setnonblocking(int fd) {
    long arg;
    if ((arg = fcntl(fd, F_GETFL, NULL)) < 0) {
	perror("fcntl(F_GETFL)");
	exit(1);
    }
    arg |= O_NONBLOCK;
    if (fcntl(fd, F_SETFL, arg) < 0) {
	perror("fcntl(F_SETFL)");
	exit(1);
    }
}

/* Set fd to be blocking. */
void setblocking(int fd) {
    long arg;
    if ((arg = fcntl(fd, F_GETFL, NULL)) < 0) {
	perror("fcntl(F_GETFL)");
	exit(1);
    }
    arg &= ~O_NONBLOCK;
    if (fcntl(fd, F_SETFL, arg) < 0) {
	perror("fcntl(F_SETFL)");
	exit(1);
    }
}

/**
 * Initiate num new non-blocking connections to addr.
 */
void open_connections(int num, const addrvec &addrs, int &current_url) {
    int i, j;

    for (i = 0; i < num; ++i) {
	// Find the first available slot.
	for (j = 0; j < fds_len; j++) {
	    if (conn_info[j].state == CONN_UNUSED)
		break;
	}
	if (j == fds_len) {
	    fprintf(stderr, "out of fds\n");
	    exit(1);
	}

	// Create the socket.
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
	    wave_stats.socket[errno]++;
	    continue;
	}

	// Select the next address;
	const struct sockaddr *addr = (struct sockaddr *) &addrs[current_url];
	conn_info[j].url_number = current_url;
	current_url = (current_url + 1) % addrs.size();

	// Use non-blocking connects which correctly fail with EINPROGRESS.
	setnonblocking(fd);
	if (! (connect(fd, (struct sockaddr *) addr, sizeof(*addr))<0
	       && errno == EINPROGRESS)) {
	    wave_stats.connect[errno]++;
	    close(fd);
	    continue;
	}

	// Record the socket. Request POLLOUT so poll() informs us on connect.
	fds[j].fd = fd;
	fds[j].events = POLLIN|POLLOUT;
	conn_info[j].state = CONN_CONNECTING;
	conn_info[j].request_number = request_count++;
	time_interval::gettod(&conn_info[j].connecting);
	wave_stats.opened++;

	if (debug)
	    printf("open: fds %d, fd %d\n", j, fd);
    }
}

/**
 * Display strerror() strings from map, prefixed by label.
 */
void report_errors(intmap &map, const char *label) {
    if (map.size() > 0) {
	std::cout << "\t" << label << ": ";
	for (intmap::iterator it = map.begin(); it != map.end(); it++) {
	    std::cout << strerror(it->first) << ":" << it->second << " ";
	}
	std::cout << std::endl;
    }
}

/**
 * Display errmap error strings from map, prefixed by label.
 */
void report_errors(strmap &errmap, intmap &map, const char *label) {
    if (map.size() > 0) {
	std::cout << "\t" << label << ": ";
	for (intmap::iterator it = map.begin(); it != map.end(); it++) {
	    std::cout << it->first << " " << errmap[it->first] << ":" << it->second << " ";
	}
	std::cout << std::endl;
    }
}

/**
 * Display events since the last reporting period, then reset the counters.
 */
void report_connections(time_interval *start) {
    static int rows = 0;

    if (rows == 0) {
	printf("     | delta      | | total | | results                   |\n"
	       "secs  new estb clos pend estb errs  200  500  503  504  xxx\n"
	       "---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ----\n");
    }
    rows++;

    static int skip_if_nothing_happened = 0;
    int nothing_happened = (wave_stats.opened == 0 &&
			    wave_stats.connected == 0 &&
			    wave_stats.closed == 0 &&
			    wave_stats.socket.size() == 0 &&
			    wave_stats.connect.size() == 0 &&
			    wave_stats.read.size() == 0 &&
			    wave_stats.write.size() == 0 &&
			    wave_stats.http_code.size() == 0);
    if (nothing_happened) {
	if (skip_if_nothing_happened) {
	    return;
	}
	else {
	    skip_if_nothing_happened = 1;
	}
    }
    else {
	skip_if_nothing_happened = 0;
    }

    intmap::iterator it;
    int connecting = 0, established = 0, errs = 0, http_errs = 0;

    // Sum all syscall errors.
    for (it = wave_stats.socket.begin(); it != wave_stats.socket.end(); it++) {
	errs += it->second;
    }
    for (it = wave_stats.connect.begin(); it != wave_stats.connect.end(); it++) {
	errs += it->second;
    }
    for (it = wave_stats.read.begin(); it != wave_stats.read.end(); it++) {
	errs += it->second;
    }
    for (it = wave_stats.write.begin(); it != wave_stats.write.end(); it++) {
	errs += it->second;
    }
    // Sum the HTTP codes to report collectively.
    for (it = wave_stats.http_code.begin(); it != wave_stats.http_code.end(); it++) {
	switch (it->first) {
	case 200:
	case 500:
	case 503:
	case 504:
	    break;
	default:
	    http_errs += it->second;
	    break;
	}
    }
    // Sum statuses.
    for (int i = 0; i < fds_len; ++i) {
	switch (conn_info[i].state) {
	case CONN_UNUSED:
	    break;
	case CONN_CONNECTING:
	    connecting++;
	    break;
	case CONN_ESTABLISHED:
	    established++;
	    break;
	}
    }

    struct timeval since;
    start->since(&since);
    printf("%4lu %4d %4d %4d %4d %4d %4d %4d %4d %4d %4d %4d\n", since.tv_sec, wave_stats.opened, wave_stats.connected, wave_stats.closed, connecting, established, errs, wave_stats.http_code[200], wave_stats.http_code[500], wave_stats.http_code[503], wave_stats.http_code[504], http_errs);
    report_errors(wave_stats.socket, "socket");
    report_errors(wave_stats.connect, "connect");
    report_errors(wave_stats.read, "read");
    report_errors(wave_stats.write, "write");
    wave_stats.http_code.erase(200);
    wave_stats.http_code.erase(500);
    wave_stats.http_code.erase(503);
    wave_stats.http_code.erase(504);
    report_errors(http_codes, wave_stats.http_code, "http");

    wave_stats.clear();

    fflush(stdout);
}

/**
 * Clean up a connection slot.
 */
void close_connection(int i) {
    close(fds[i].fd);
    wave_stats.closed++;
    fds[i].fd = fds[i].events = 0;
    conn_info[i].state = CONN_UNUSED;

    // poll() can return POLLIN with an empty read and POLLHUP at the
    // same time. Prevent this from being called again.
    fds[i].revents &= ~(POLLIN | POLLHUP);
}

/**
 * Get the socket error for a connection slot.
 */
int get_sock_error(int i) {
    int optval;
    socklen_t optlen;
    optlen = sizeof(optval);
    if (getsockopt(fds[i].fd, SOL_SOCKET, SO_ERROR, &optval, &optlen)< 0) {
	perror("getsockopt(SO_ERROR, SOL_SOCKET");
	exit(1);
    }
    return optval;
}

/**
 * Initialize the table of HTTP response code strings.
 */
void init_http_codes() {
    http_codes[100] = "Continue";
    http_codes[101] = "Switching Protocols";
    http_codes[200] = "OK";
    http_codes[201] = "Created";
    http_codes[202] = "Accepted";
    http_codes[203] = "Non-Authoritative Information";
    http_codes[204] = "No Content";
    http_codes[205] = "Reset Content";
    http_codes[206] = "Partial Content";
    http_codes[300] = "Multiple Choices";
    http_codes[301] = "Moved Permanently";
    http_codes[302] = "Found";
    http_codes[303] = "See Other";
    http_codes[304] = "Not Modified";
    http_codes[305] = "Use Proxy";
    http_codes[306] = "(Unused)";
    http_codes[307] = "Temporary Redirect";
    http_codes[400] = "Bad Request";
    http_codes[401] = "Unauthorized";
    http_codes[402] = "Payment Required";
    http_codes[403] = "Forbidden";
    http_codes[404] = "Not Found";
    http_codes[405] = "Method Not Allowed";
    http_codes[406] = "Not Acceptable";
    http_codes[407] = "Proxy Authentication Required";
    http_codes[408] = "Request Timeout";
    http_codes[409] = "Conflict";
    http_codes[410] = "Gone";
    http_codes[411] = "Length Required";
    http_codes[412] = "Precondition Failed";
    http_codes[413] = "Request Entity Too Large";
    http_codes[414] = "Request-URI Too Long";
    http_codes[415] = "Unsupported Media Type";
    http_codes[416] = "Requested Range Not Satisfiable";
    http_codes[417] = "Expectation Failed";
    http_codes[500] = "Internal Server Error";
    http_codes[501] = "Not Implemented";
    http_codes[502] = "Bad Gateway";
    http_codes[503] = "Service Unavailable";
    http_codes[504] = "Gateway Timeout";
    http_codes[505] = "HTTP Version Not Supported";
}

int main(int argc, char **argv) {
    time_interval wave_interval("wave"), report_interval("report"), start("start");
    const char *wave_spec, *p;
    char ch;
    int num, stop_after, unique, wave_limit, no_wave_limit;
    rlim_t max_fds;
    strvec headers;

    num = debug = stop_after = unique = wave_limit = 0;
    no_wave_limit = 1;
    // default wave spec is just one wave
    wave_spec = "1000:1";
    max_fds = 256;
    while ((ch = getopt(argc, argv, "un:t:r:df:m:h:")) != -1) {
	switch (ch) {
	case 'u':
	    unique = 1;
	    break;
	case 'n':
	    num = atoi(optarg);
	    break;
	case 't':
	    wave_spec = optarg;
	    break;
	case 'r':
	    report_interval.set(atoi(optarg)*1000);
	    break;
	case 'd':
	    debug++;
	    break;
	case 'f':
	    max_fds = atoi(optarg);
	    break;
	case 'm':
	    stop_after = atoi(optarg);
	    break;
	case 'h':
	    headers.push_back(optarg);
	    break;
	default:
	    usage();
	}
    }

    wave_interval.set(atoi(wave_spec)*1000);
    p = strchr(wave_spec, ':');
    if (p != NULL) {
	wave_limit = atoi(p+1);
	no_wave_limit = 0;
    }

    if (report_interval.get() == 0) {
	report_interval.set(wave_interval.get());
    }
    if (num == 0 || wave_interval.get() == 0 || report_interval.get() == 0) {
	usage();
    }

    argc -= optind;
    argv += optind;
    if (argc < 1)
	usage();

    urlvec urls;
    while (*argv) {
      url url(*argv++);
      urls.push_back(url);
    }
    url url = urls[0];

    // Decide how many fds we can use.
    struct rlimit rlim;
    if (getrlimit(RLIMIT_NOFILE, &rlim) < 0) {
	perror("getrlimit");
	exit(1);
    }
    rlim.rlim_cur = std::max(rlim.rlim_cur, max_fds);
    rlim.rlim_max = std::max(rlim.rlim_max, max_fds);
    if (setrlimit(RLIMIT_NOFILE, &rlim) < 0) {
	perror("setrlimit");
	exit(1);
    }

    // Allocate/initialize various data structures.
    fds_len = rlim.rlim_cur;
    fds = (struct pollfd *)calloc(fds_len, sizeof(struct pollfd));
    conn_info = (struct conn_info_t *)calloc(fds_len, sizeof(struct conn_info_t));
    init_http_codes();
    wave_stats.clear();

    addrvec addrs;
    for (int i = 0; i < urls.size(); i++) {
      struct hostent *h = gethostbyname(urls[i].host().c_str());
      if (h == NULL) {
	fprintf(stderr, "cannot resolve host: %s\n", urls[i].host().c_str());
	exit(1);
      }

      struct sockaddr_in addr;
      memset(&addr, 0, sizeof(addr));
      addr.sin_family = AF_INET;
      addr.sin_port = htons(url.port());
      memcpy(&addr.sin_addr.s_addr, h->h_addr, h->h_length);
      addrs.push_back(addr);
    }

    int current_url = 0;

    // Mark time and kick off the first wave.
    struct timeval now;
    time_interval::gettod(&now);
    start.mark(&now);
    if (no_wave_limit || wave_limit-- > 0) {
      open_connections(num, addrs, current_url);
    }
    report_connections(&start);

    int wait_interval = std::min(wave_interval.get(), report_interval.get())/1000;
    while (1) {
	if (stop_after > 0) {
	    struct timeval since;
	    start.since(&since);
	    if (since.tv_sec > stop_after) {
		break;
	    }
	}

	int nfds = poll(fds, fds_len, wait_interval);
	if (nfds < 0) {
	    perror("poll");
	    exit(1);
	}

	time_interval::gettod(&now);
	if (wave_interval.passed(&now)) {
	    if (no_wave_limit || wave_limit-- > 0) {
	      open_connections(num, addrs, current_url);
	    }
	    wave_interval.mark(&now);
	}

	if (report_interval.passed(&now)) {
	    report_connections(&start);
	    report_interval.mark(&now);
	}

	if (nfds == 0) {
	    continue;
	}

	for (int i = 0; i < fds_len; ++i) {
	    // Presumably a non-blocking connect error?
	    if (fds[i].revents & POLLERR) {
		fds[i].revents &= ~POLLERR;

		int err = get_sock_error(i);
		wave_stats.connect[err]++;
		close_connection(i);
		if (debug)
		    printf("fd %d err: %d\n", fds[i].fd, err);
	    }

	    // Non-blocking connect succeeded or failed.
	    if (fds[i].revents & POLLOUT) {
		fds[i].revents &= ~POLLOUT;

		int err = get_sock_error(i);
		if (err == 0) {
		    // Connect succeeded. Stop polling for write.
		    fds[i].events &= ~POLLOUT;
		    wave_stats.connected++;
		    conn_info[i].state = CONN_ESTABLISHED;
		    time_interval::gettod(&conn_info[i].connected);

		    // For now, use blocking IO.
		    setblocking(fds[i].fd);
		    if (debug)
			printf("fd %d: connect\n", fds[i].fd);

		    struct timeval diff;
		    time_interval::timeval_subtract(&diff, &conn_info[i].connected, &conn_info[i].connecting);
		    time_t delta = diff.tv_sec*1000000+diff.tv_usec;
		    if (delta > 1000000) {
			printf("%d connect time: %lu\n", conn_info[i].request_number, delta);
		    }

		    // Build the URL and request headers.
		    char request[8192];
		    int found_ua = 0, found_host = 0;
		    sprintf(request, "GET %s", urls[conn_info[i].url_number].request().c_str());
		    if (unique) {
			sprintf(request+strlen(request), "&cnt=%d", conn_info[i].request_number);
		    }
		    strcat(request, " HTTP/1.0\r\n");
		    for (strvec::iterator it = headers.begin(); it != headers.end(); it++) {
			strcat(request, it->c_str());
			strcat(request, "\r\n");
			if (strcasestr(it->c_str(), "host:") != NULL) {
			    found_host = 1;
			}
			if (strcasestr(it->c_str(), "user-agent:") != NULL) {
			    found_ua = 1;
			}
		    }
		    if (! found_host) {
			strcat(request, "Host: ");
			strcat(request, urls[conn_info[i].url_number].host().c_str());
			strcat(request, "\r\n");
		    }
		    if (! found_ua) {
			strcat(request, "User-Agent: Goofy 0.0\r\n");
		    }
		    strcat(request, "\r\n");
		    if (debug)
			printf("%s", request);

		    int request_len = strlen(request);

		    // Send the request.
		    if (write(fds[i].fd, request, request_len) != request_len) {
			// We can't write the request to the socket, give up.
			wave_stats.write[errno]++;
			close_connection(i);
			if (debug)
			    printf("fd %d: write err: %d\n", fds[i].fd, err);
		    }
		}
		else {
		    // Connect failed.
		    wave_stats.connect[err]++;
		    close_connection(i);
		    if (debug)
			printf("fd %d: connect err: %d\n", fds[i].fd, err);
		}
	    }

	    // Data available. Read just once so we never block. If there is
	    // more data, we'll get it next time.
	    if (fds[i].revents & POLLIN) {
		fds[i].revents &= ~POLLIN;

		char buf[8192];
		int n = read(fds[i].fd, buf, sizeof(buf));
		if (n < 0) {
		    wave_stats.read[errno]++;
		    close_connection(i);
		    if (debug)
			printf("fd %d read err: %d\n", fds[i].fd, errno);
		    continue;
		}
		else if (n == 0) {
		    // No data means peer closed the connection.
		    if (debug)
			printf("fd %d empty read\n", fds[i].fd);
		    close_connection(i);
		}
		else {
		    buf[n-1] = 0;
		    if (debug > 1)
			printf("fd %d read: %s\n", fds[i].fd, buf);
		    if (strstr(buf, "HTTP/1.") == buf) {
			wave_stats.http_code[atoi(buf+9)]++;
		    }
		}
	    }

	    // Peer closed the connection.
	    if (fds[i].revents & POLLHUP) {
		fds[i].revents &= ~POLLHUP;
		if (debug)
		    printf("fd %d closed\n", fds[i].fd);
		close_connection(i);
	    }

	    // Anything left in revents is unexpected.
	    if (fds[i].revents) {
		printf("fd %d: 0x%x\n", fds[i].fd, fds[i].revents);
	    }
	}
    }

    return 0;
}
