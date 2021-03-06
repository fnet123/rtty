/*
 * Copyright (C) 2017 Jianhui Zhao <jianhuizhao329@gmail.com>
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>
#include <pty.h>
#include <uwsc/uwsc.h>
#include <libubox/blobmsg_json.h>

#include "utils.h"

#define KEEPALIVE_INTERVAL  10

struct tty_session {
    pid_t pid;
    int pty;
    char sid[33];
    struct ustream_fd sfd;
    struct uloop_process up;
    struct list_head node;
};

struct uwsc_client *cl;

static char buf[4096 * 10];
static struct blob_buf b;
static char mac[13];
static char login[128];
static char server_url[128] = "";
static bool auto_reconnect;

static LIST_HEAD(tty_sessions);

enum {
    RTTYD_TYPE,
    RTTYD_MAC,
    RTTYD_SID,
    RTTYD_DATA
};

static const struct blobmsg_policy pol[] = {
    [RTTYD_TYPE] = {
        .name = "type",
        .type = BLOBMSG_TYPE_STRING,
    },
    [RTTYD_MAC] = {
        .name = "mac",
        .type = BLOBMSG_TYPE_STRING,
    },
    [RTTYD_SID] = {
        .name = "sid",
        .type = BLOBMSG_TYPE_STRING,
    },
    [RTTYD_DATA] = {
        .name = "data",
        .type = BLOBMSG_TYPE_STRING,
    }
};

static void reconnect()
{
TRY:
    cl = uwsc_new(server_url);
    if (!cl) {
        sleep(5);
        goto TRY;
    }
}

static void keepalive(struct uloop_timeout *utm)
{
    char *str;

    blobmsg_buf_init(&b);
    blobmsg_add_string(&b, "type", "ping");
    blobmsg_add_string(&b, "mac", mac);

    str = blobmsg_format_json(b.head, true);
    cl->send(cl, str, strlen(str), WEBSOCKET_OP_TEXT);
    free(str);

    uloop_timeout_set(utm, KEEPALIVE_INTERVAL * 1000);
}

static void del_tty_session(struct tty_session *tty)
{
    list_del(&tty->node);
    uloop_process_delete(&tty->up);
    ustream_free(&tty->sfd.stream);
    close(tty->pty);
    kill(tty->pid, SIGTERM);
    waitpid(tty->pid, NULL, 0);
}

static void pty_read_cb(struct ustream *s, int bytes)
{
    struct tty_session *tty = container_of(s, struct tty_session, sfd.stream);
    char *str;
    int len;

    str = ustream_get_read_buf(s, &len);
    
    blobmsg_buf_init(&b);
    blobmsg_add_string(&b, "type", "data");
    blobmsg_add_string(&b, "mac", mac);
    blobmsg_add_string(&b, "sid", tty->sid);

    b64_encode(str, len, buf, sizeof(buf));
    ustream_consume(s, len);

    blobmsg_add_string(&b, "data", buf);

    str = blobmsg_format_json(b.head, true);
    cl->send(cl, str, strlen(str), WEBSOCKET_OP_TEXT);
    free(str);
}

static void pty_on_exit(struct uloop_process *p, int ret)
{
    struct tty_session *tty = container_of(p, struct tty_session, up);
    char *str;

    blobmsg_buf_init(&b);
    blobmsg_add_string(&b, "type", "logout");
    blobmsg_add_string(&b, "mac", mac);
    blobmsg_add_string(&b, "sid", tty->sid);

    str = blobmsg_format_json(b.head, true);
    cl->send(cl, str, strlen(str), WEBSOCKET_OP_TEXT);
    free(str);

    del_tty_session(tty);
}

static void new_tty_session(struct blob_attr **tb)
{
    struct tty_session *s;
    int pty;
    pid_t pid;

    s = calloc(1, sizeof(struct tty_session));
    if (!s)
        return;

    pid = forkpty(&pty, NULL, NULL, NULL);
    if (pid == 0)
        execl(login, login, NULL);

    s->pid = pid;
    s->pty = pty;
    memcpy(s->sid, blobmsg_get_string(tb[RTTYD_SID]), 32);
    
    list_add(&s->node, &tty_sessions);

    s->sfd.stream.notify_read = pty_read_cb;
    ustream_fd_init(&s->sfd, s->pty);

    s->up.pid = pid;
    s->up.cb = pty_on_exit;
    uloop_process_add(&s->up);
}

static void uwsc_onopen(struct uwsc_client *cl)
{
    uwsc_log_debug("onopen");
}

static void uwsc_onmessage(struct uwsc_client *cl, char *msg, uint64_t len, enum websocket_op op)
{
    struct blob_attr *tb[ARRAY_SIZE(pol)];
    const char *type;

    blobmsg_buf_init(&b);

    blobmsg_add_json_from_string(&b, msg);

    if (blobmsg_parse(pol, ARRAY_SIZE(pol), tb, blob_data(b.head), blob_len(b.head)) != 0) {
        fprintf(stderr, "Parse failed\n");
        return;
    }

    if (!tb[RTTYD_TYPE])
        return;

    if (!tb[RTTYD_MAC])
        return;

    if (!tb[RTTYD_SID])
        return;

    type = blobmsg_get_string(tb[RTTYD_TYPE]);
    if (!strcmp(type, "login")) {
        new_tty_session(tb);
    } else if (!strcmp(type, "logout")) {
        const char *sid = blobmsg_get_string(tb[RTTYD_SID]);
        struct tty_session *tty, *tmp;

        list_for_each_entry_safe(tty, tmp, &tty_sessions, node) {
            if (!strcmp(tty->sid, sid)) {
                del_tty_session(tty);
            }
        }
    } else if (!strcmp(type, "data")) {
        const char *sid = blobmsg_get_string(tb[RTTYD_SID]);
        const char *data = blobmsg_get_string(tb[RTTYD_DATA]);
        struct tty_session *tty;
        int len;

        len = b64_decode(data, buf, sizeof(buf));
        list_for_each_entry(tty, &tty_sessions, node) {
            if (!strcmp(tty->sid, sid)) {
                if (write(tty->pty, buf, len) < 0)
                    perror("write");
                break;
            }
        }
    }
}

static void uwsc_onerror(struct uwsc_client *cl)
{
    uwsc_log_err("onerror:%d", cl->error);
}

static void uwsc_onclose(struct uwsc_client *cl)
{
    uwsc_log_debug("onclose");

    if (auto_reconnect)
        reconnect();
    else
        uloop_end();
}

static int find_login()
{
    FILE *fp = popen("which login", "r");
    if (fp) {
        if (fgets(login, sizeof(login), fp))
            login[strlen(login) - 1] = 0;
        pclose(fp);

        if (!login[0])
            return -1;
        return 0;
    }

    return -1;
}

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [option]\n"
        "      -i ifname    # Network device name\n"
        "      -h host      # Server host\n"
        "      -p port      # Server port\n"
        "      -a           # Auto reconnect to the server\n"
        , prog);
    exit(1);
}

int main(int argc, char **argv)
{
    int opt;
    const char *host = NULL;
    int port = 0;

    struct uloop_timeout keepalive_timer = {
        .cb = keepalive
    };

    if (setuid(0) < 0) {
        fprintf(stderr, "Operation not permitted\n");
        return -1;
    }

    if (find_login() < 0) {
        fprintf(stderr, "The program 'login' is not found\n");
        return -1;
    }

    while ((opt = getopt(argc, argv, "i:h:p:a")) != -1) {
        switch (opt)
        {
        case 'i':
            if (get_iface_mac(optarg, mac, sizeof(mac)) < 0) {
                return -1;
            }
            break;
        case 'h':
            host = optarg;
            break;
        case 'p':
            port = atoi(optarg);
            break;
        case 'a':
            auto_reconnect = true;
            break;
        default: /* '?' */
            usage(argv[0]);
        }
    }

    if (!mac[0] || !host || !port)
        usage(argv[0]);

    uloop_init();

    snprintf(server_url, sizeof(server_url), "ws://%s:%d/ws/device?mac=%s", host, port, mac);

    /*
    ** uwsc_new() will try to connect, and if more than 5 seconds have not been
    ** connected to success, it will return
    */
TRY:
    cl = uwsc_new(server_url);
    if (!cl) {
        if (auto_reconnect) {
            sleep(5);
            goto TRY;
        }
        return -1;
    }
   
    cl->onopen = uwsc_onopen;
    cl->onmessage = uwsc_onmessage;
    cl->onerror = uwsc_onerror;
    cl->onclose = uwsc_onclose;
    
    keepalive(&keepalive_timer);

    uloop_run();

    cl->send(cl, NULL, 0, WEBSOCKET_OP_CLOSE);
    cl->free(cl);

    uloop_done();
    
    return 0;
}
