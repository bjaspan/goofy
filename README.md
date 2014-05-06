Goofy is an HTTP load testing tool that simulates waves of surfers in
an unusual way (get it? ask a surfer/boarder).

Other HTTP load testing tools (such as ab and siege) perform an operation as
quickly as possible and report server performance. The number of concurrent
connections is specified and the connection rate is measured. The outcome of
operations is summarized when the pre-defined test period is complete.

Goofy does the opposite of that. It initiates operations at a specified rate
and concurrency, and reports the outcome of each operation regardless of how
long it takes. It shows the status of operations as they occur, so it is
possible to determine the timing of intermediate steps.

More specifically, Goofy initiates a fixed number of connections to a
URL every specified time period, letting them all run in parallel
until they finish. Each time period, it reports on the number of
connections pending, established, and closed, as well as how
connections closed (syscall error or HTTP status).

## Usage

```
Usage: goofy [args] url
  -n num           number of requests per wave
  -t ms[:limit]    milliseconds between waves; run limit total waves;
                   default to one wave
  -r ms            milliseconds between reports; defaults to -t or 1000
  -m secs          total seconds to run test; default is unlimited
  -f fds           maximum number of sockets to request from the os
  -h hdr           add hdr ("Header: value") to each request
  -d               debug
```

## Quick start

Let's test whether Google handle 3 page requests at a time.

```
$ goofy -n 3 -t 2000 -r 1000 http://www.google.com/
```

-n specifies the number of requests per wave, -t specifies the time between
waves (ms), and -r specifies the reporting period (ms). In this case, we're
sending 3 requests every 2 seconds, but reporting every second. Here's the
output:

```
     | delta      | | total | | results                   |
secs  new estb clos pend estb errs  200  500  503  504  xxx
---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ----
   0    3    0    0    3    0    0    0    0    0    0    0
   1    0    3    3    0    0    0    3    0    0    0    0
   2    3    0    0    3    0    0    0    0    0    0    0
   3    0    3    3    0    0    0    3    0    0    0    0
```

The "delta" group shows what happened during the current reporting period. new
is initiated but not yet established connections; estb is established
connections, and clos is closed connections.

The "total" group counts the status of all open but not yet complete
connections. pend is initiated but not yet established, and estb is connected
but not yet closed.

The "results" group shows how many connections ended during that period and
with what result. errs shows socket API errors (e.g. ECONNREFUSED), and the
other columns show HTTP statuses.

At time 0, we see 3 newly opened connections in the new column. They have not
connected yet, so delta estb shows 0, and pend shows 3. No requests have
completed.

At time 1, the delta-estb and clos(ed) columns both show 3, indicating that the
3 requests from the first wave got established, completed, and closed since
time 0. At that instant, no connections were still open, so the pend and total-
estb columns show zero; if a request had taken longer than 1 second, it would
still be open and total-estb would show it. We also see that all 3 connections
that closed got HTTP status 200.

At time 2, the second wave starts, and completes at time 3.

So yes, Google can handle 3 requests at a time. :-)

## Test: PHP-FPM process launching

[ This experiment was conducted in 2014. ]

I noticed a problem with how the PHP-FPM ondemand process manager spawns worker
processes. PHP-FPM is running under Apache, and is configured with:

```
pm = ondemand
pm.max_children = 100
pm.process_idle_timeout = 10s
```

To test it, I ran just one wave of 10 requests, each to a PHP script that
sleeps for 3 seconds. By default, goofy reports results every second. Here's
what happened:

```
$ ./goofy -n 10 'http://server/sleep.php?sleep=3'
     | delta      | | total | | results                   |
secs  new estb clos pend estb errs  200  500  503  504  xxx Notes added by hand
---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- -----------------------------------------------------
   0   10    0    0   10    0    0    0    0    0    0    0 10 connections opened, all still pending.
   1    0   10    0    0   10    0    0    0    0    0    0 10 connections became established.
   2    0    0    0    0   10    0    0    0    0    0    0 All 10 connections are still established.
   4    0    0    4    0    6    0    4    0    0    0    0 Between 3-4s, *four* requests complete. Why not 10?
   5    0    0    0    0    6    0    0    0    0    0    0
   7    0    0    4    0    2    0    4    0    0    0    0 Between 6-7s, four more complete.
   8    0    0    0    0    2    0    0    0    0    0    0
  10    0    0    2    0    0    0    2    0    0    0    0 Between 9-10s, the last two complete.
  11    0    0    0    0    0    0    0    0    0    0    0
```

PHP-FPM should have spawned 10 processes, but this shows that it only spawned
four; ps on the server confirms this observation. Even when the first set of
requests finished and there were still six pending, still no new workers were
spawned.

Clearly, PHP-FPM is not behaving correctly. This data helped me track down the
bug in the ondemand process manager; a fix is forthcoming.

## A lengthy example: A History of a Thousand Connections

[ This experiment was conducted in 2010. ]

With an Apache web server running PHP under FastCGI using mod_fcgid, I
wanted to understand exactly how the number of Apache processes,
php-cgi processes, and the various timeouts affected how HTTP requests
get served.

I accomplished this by having Goofy initiate 1,000 simultaneous
connections to a URL that just sleeps for 60 seconds and waits for
them all to finish one way or another. The command line is:

```
$ ./goofy -n 1000 -r 2000 'http://server/sleep.php?sleep=60'
```

These options say to open 1,000 connections and to report results
every 2 seconds. Goofy produces no output if nothing happens during a
particular reporting interval.

For this test, the server was an EC2 m1.small instance configured with
256 Apache processes but only 10 php-cgi processes. I learned:

* The server kernel/Apache accepts more than one TCP
  connection per Apache process, but not even as many as two per
  Apache process. So the "listen queue" is not behaving as I
  expect. It seems like we should expect to get at least 1,280
  established TCP connections to a server with 256 Apache processes
  before connection errors occur.

* The lucky requests that actually get through to a PHP process work
  even under these conditions.

* mod_fcgi consistently kicks out PHP requests that cannot get a PHP
  process after 65 seconds.

```
     | delta      | | total | | results              |
secs  new estb clos totl  err  200  500  503  504  xxx Notes added by hand
---- ---- ---- ---- ---- ---- ---- ---- ---- ---- ---- -----------------------------------------------------
   0 1000    0    0 1000    0    0    0    0    0    0 1000 new non-blocking connections opened.
   2    0  256    0 1000    0    0    0    0    0    0 256 of the non-blocking connections actually connect.
   5    0  256    0 1000    0    0    0    0    0    0 256 more connect.
  11    0   30    0 1000    0    0    0    0    0    0 30 more connect.
  23    0   66    0 1000    0    0    0    0    0    0 66 more connect. We're up 608 TCP connections for 256 Apache kids.
  38    0    0   99  901   99    0    0    0    0    0 The remote kernel kicks out 99 requests with "connection reset".
    connect: Connection reset by peer:99               Why not the other 293 also? I have no idea.
  47    0    2    0  901    0    0    0    0    0    0 No Apache has released its socket, yet now 2 more connects complete!
  61    0    0    1  900    0    1    0    0    0    0 It's been 60 secs so the 10 lucky connections...
  64    0    0    9  891    0    9    0    0    0    0 ...that got a php-cgi complete with status 200.
  67    0    0  142  749    0    0    0  142    0    0 At 65 secs, mod_fcigd times out pending reqs with a 503.
  69    0    0    2  747    0    0    0    2    0    0 More mod_fcgid 503s...
  71    0    0    2  745    0    0    0    2    0    0 more
  73    0    0    2  743    0    0    0    2    0    0 more
  76    0    0    2  741    0    0    0    2    0    0 more
  78    0    0    5  736    0    0    0    5    0    0 more
  80    0    0   12  724    0    0    0   13    0    0 more
  82    0    0   14  710    0    0    0   13    0    0 more
  85    0    0   30  680    0    0    0   30    0    0 more
  87    0    0   32  648    0    0    0   32    0    0 more
  89    0    0    1  647    0    0    0    1    0    0 more
```

So, after 90 seconds, at the TCP level we've seen 610 connections and
99 connection resets. After all that, we had the 10 status 200
completions, then mod_fcgid kicked out 244 requests with status 503.

```
  95    0  282    0  647    0    0    0    0    0    0 The server accepts 282 more TCP connections.
 121    0    0    1  646    0    1    0    0    0    0 It's been 120 seconds. 10 more lucky php requests complete.
 124    0    0    9  637    0    9    0    0    0    0
 127    0    0    5  632    4    0    0    1    0    0 4 TCP resets, and (at about 65*2 secs) the 503s start again.
    connect: Connection reset by peer:4
 129    0    0    6  626    5    0    0    1    0    0 more of both
    connect: Connection reset by peer:5
 133    0    0  142  484   22    0    0  120    0    0 more of both
    connect: Connection reset by peer:22
 135    0    0    3  481    0    0    0    3    0    0 503s forever!
 137    0    0    2  479    0    0    0    2    0    0 more
 139    0    0    2  477    0    0    0    2    0    0 more
 141    0    0    2  475    0    0    0    2    0    0 more
 144    0    0    7  468    0    0    0    7    0    0 more
 146    0    0   19  449    0    0    0   19    0    0 more
 148    0    0   15  434    0    0    0   15    0    0 more
 150    0    0   41  393    0    0    0   41    0    0 more
 152    0    0   31  362    0    0    0   31    0    0 more
 154    0    0    1  361    0    0    0    1    0    0 more
 181    0    0    1  360    0    1    0    0    0    0 Oh, 180 seconds! 10 more luck php requests complete.
 184    0    0    9  351    0    9    0    0    0    0
 189    0    0    1  350    0    0    0    1    0    0 The 65*3 secs 503s start.
 191    0    0  108  242  108    0    0    0    0    0 Now our LOCAL KERNEL is saying "connection timed out" for 108 reqs.
    connect: Connection timed out:108
 194    0    0    2  240    0    0    0    2    0    0 503s...
 196    0    0   29  211    0    0    0   29    0    0 more
 198    0    0   86  125    0    0    0   86    0    0 more
 200    0    0    3  122    0    0    0    3    0    0 more
 202    0    0    2  120    0    0    0    2    0    0 more
 204    0    0    1  119    0    0    0    1    0    0 more
 208    0    0    1  118    0    0    0    1    0    0 more
 227    0    0   17  101    0    0    0   17    0    0 more
 230    0    0   20   81    0    0    0   20    0    0 more
 232    0    0   34   47    0    0    0   34    0    0 more
 234    0    0   37   10    0    0    0   37    0    0 more
 241    0    0    1    9    0    1    0    0    0    0 At four minutes, the last 10 luck php requests complete.
 244    0    0    9    0    0    9    0    0    0    0 And now we're done.
```
