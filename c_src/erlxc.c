/* Copyright (c) 2013-2014, Michael Santos <michael.santos@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include "erlxc.h"
#include "erlxc_version.h"

#define ERLXC_MSG_SYNC  0
#define ERLXC_MSG_ASYNC (htons(1))

static void erlxc_loop(erlxc_state_t *);
static void usage(erlxc_state_t *);

static int erlxc_write(u_int16_t, ETERM *);
static ssize_t erlxc_read(void *, ssize_t);

static void erlxc_stats(erlxc_state_t *ep);

extern char *__progname;

    int
main(int argc, char *argv[])
{
    erlxc_state_t *ep = NULL;
    char *name = NULL;
    char *path = NULL;
    char *errlog = NULL;
    int ch = 0;

    erl_init(NULL, 0);

    ep = calloc(1, sizeof(erlxc_state_t));
    if (!ep)
        erl_err_sys("calloc");

    ep->opt |= erlxc_opt_stop_on_exit;
    ep->opt |= erlxc_opt_daemonize;
    ep->opt |= erlxc_opt_closeallfds;

    while ( (ch = getopt(argc, argv, "d:hn:o:P:t:v")) != -1) {
        switch (ch) {
            case 'd':
                if (strcmp("nodaemonize", optarg) == 0)
                    ep->opt &= ~erlxc_opt_daemonize;
                else if (strcmp("nocloseallfds", optarg) == 0)
                    ep->opt &= ~erlxc_opt_closeallfds;
                break;
            case 'n':
                name = strdup(optarg);
                if (!name)
                    erl_err_sys("name");
                break;
            case 'o':
                errlog = strdup(optarg);
                if (!errlog)
                    erl_err_sys("errlog");
                break;
            case 'P':
                path = strdup(optarg);
                if (!path)
                    erl_err_sys("path");
                break;
            case 't':
                if (strcmp("temporary", optarg) == 0) {
                    ep->opt |= erlxc_opt_stop_on_exit;
                    ep->opt |= erlxc_opt_destroy_on_exit;
                }
                else if (strcmp("transient", optarg) == 0) {
                    ep->opt |= erlxc_opt_stop_on_exit;
                    ep->opt &= ~erlxc_opt_destroy_on_exit;
                }
                else if (strcmp("permanent", optarg) == 0) {
                    ep->opt &= ~erlxc_opt_stop_on_exit;
                    ep->opt &= ~erlxc_opt_destroy_on_exit;
                }
                else
                    usage(ep);
                break;
            case 'v':
                ep->verbose++;
                break;
            case 'h':
            default:
                usage(ep);
        }
    }

    if (!name)
        usage(ep);

    if (errlog) {
        if (!freopen(errlog, "w+", stderr))
            erl_err_sys("freopen");
    }

    ep->c = lxc_container_new(name, path);
    if (!ep->c)
        erl_err_quit("failed to create container");

    free(name);
    free(path);

    erlxc_loop(ep);

    if (ep->opt & erlxc_opt_stop_on_exit) {
        VERBOSE(1, "stopping container:%s", ep->c->name);
        (void)ep->c->stop(ep->c);
        (void)ep->c->wait(ep->c, "STOPPED", -1);
    }

    if (ep->opt & erlxc_opt_destroy_on_exit) {
        VERBOSE(1, "destroying container:%s", ep->c->name);
        (void)ep->c->destroy(ep->c);
    }

    exit(0);
}

    static void
erlxc_loop(erlxc_state_t *ep)
{
    unsigned char buf[UINT16_MAX] = {0};
    unsigned char *msg = buf;
    ETERM *arg = NULL;
    ETERM *reply = NULL;
    u_int16_t len = 0;
    u_int16_t cmd = 0;

    for ( ; ; ) {
        if (erlxc_read(&len, sizeof(len)) != sizeof(len))
            return;

        len = ntohs(len);

        if (len <= sizeof(cmd) || len > sizeof(buf))
            return;

        if (erlxc_read(msg, len) != len)
            return;

        cmd = get_int16(msg);
        msg += 2;
        len -= 2;

        arg = erl_decode(msg);
        if (!arg)
            erl_err_quit("invalid message");

        reply = erlxc_cmd(ep, cmd, arg);
        if (!reply)
            erl_err_quit("unrecoverable error");

        erl_free_compound(arg);

        if (erlxc_write(ERLXC_MSG_SYNC, reply) < 0)
            erl_err_sys("erlxc_write");

        erl_free_compound(reply);

        if (ep->verbose > 1)
            erlxc_stats(ep);

        (void)fflush(stderr);
    }
}

    int
erlxc_send(ETERM *t)
{
    return erlxc_write(ERLXC_MSG_ASYNC, t);
}

    static int
erlxc_write(u_int16_t type, ETERM *t)
{
    struct iovec iov[3];

    u_int16_t len;
    unsigned char buf[UINT16_MAX-4];
    int buflen = 0;

    buflen = erl_term_len(t);
    if (buflen < 0 || buflen > sizeof(buf))
        return -1;

    if (erl_encode(t, buf) < 1)
        return -1;

    len = htons(sizeof(type) + buflen);

    iov[0].iov_base = &len;
    iov[0].iov_len = sizeof(len);
    iov[1].iov_base = &type;
    iov[1].iov_len = sizeof(type);
    iov[2].iov_base = buf;
    iov[2].iov_len = buflen;

    return writev(STDOUT_FILENO, iov, sizeof(iov)/sizeof(iov[0]));
}

    static ssize_t
erlxc_read(void *buf, ssize_t len)
{
    ssize_t i = 0;
    ssize_t got = 0;

    do {
        if ((i = read(STDIN_FILENO, buf + got, len - got)) <= 0)
            return(i);
        got += i;
    } while (got < len);

    return len;
}

    static void
erlxc_stats(erlxc_state_t *ep)
{
    unsigned long allocated = 0;
    unsigned long freed = 0;

    erl_eterm_statistics(&allocated, &freed);
    VERBOSE(0, "allocated=%ld, freed=%ld", allocated, freed);
    erl_eterm_release();
}

    static void
usage(erlxc_state_t *ep)
{
    (void)fprintf(stderr, "%s %s (lxc %s)\n",
            __progname, ERLXC_VERSION, lxc_get_version());
    (void)fprintf(stderr,
            "usage: %s -n <name> <options>\n"
            "    -n <name>        container name\n"
            "    -o <path>        error log\n"
            "    -P <path>        LXC path\n"
            "    -v               verbose mode\n"
            "    -d <option>      debug: nodaemonize, nocloseallfds\n"
            "    -t <type>        container type (permanent, transient, temporary)\n",
            __progname
            );

    exit (EXIT_FAILURE);
}
