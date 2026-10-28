/*
 * SIRIL HarmonyOS control server — one TCP port, JSON line protocol.
 *
 * A deliberately small POSIX-socket server: it accepts line-delimited JSON
 * requests, forwards SIRIL commands to the embedded engine and answers with
 * one JSON object per line.  Plain text lines are also accepted as commands so
 * the port can be driven with telnet/netcat during debugging.
 *
 * Protocol
 *   request : {"id":"42","op":"exec","cmd":"load image.fit","token":"..."}
 *   reply   : {"id":"42","ok":true,"code":0,"output":"...","status":{...}}
 *
 *   ops: ping | status | version | catalog | exec | chdir
 *
 * Copyright (C) 2026 team free-astro (see more in AUTHORS file)
 *
 * Siril is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <glib.h>

#include "ohos/siril_ohos.h"

#if defined(__OHOS__)
#include <hilog/log.h>
#define OHOS_LOG_INFO(...) OH_LOG_Print(LOG_APP, LOG_INFO, 0x5349, "SIRIL", __VA_ARGS__)
#define OHOS_LOG_ERROR(...) OH_LOG_Print(LOG_APP, LOG_ERROR, 0x5349, "SIRIL", __VA_ARGS__)
#else
#define OHOS_LOG_INFO(...) g_print(__VA_ARGS__)
#define OHOS_LOG_ERROR(...) g_printerr(__VA_ARGS__)
#endif

#define MAX_REQUEST 65536

typedef struct {
	int fd;
	gchar *peer;
} ClientCtx;

static GThread *accept_thread = NULL;
static GMutex server_mutex;
static gchar *server_token = NULL;
static gchar *server_bind = NULL;
static int server_port = 0;
static int listen_fd = -1;
static gint server_clients = 0;
static gboolean server_running = FALSE;
static gint client_seq = 0;

/* ------------------------------------------------------------------ */
/* Minimal JSON helpers                                                */
/* ------------------------------------------------------------------ */

static gchar *json_unescape(const gchar *s, gsize len) {
	GString *out = g_string_new(NULL);
	for (gsize i = 0; i < len; i++) {
		if (s[i] != '\\' || i + 1 >= len) {
			g_string_append_c(out, s[i]);
			continue;
		}
		switch (s[++i]) {
		case 'n': g_string_append_c(out, '\n'); break;
		case 'r': g_string_append_c(out, '\r'); break;
		case 't': g_string_append_c(out, '\t'); break;
		case '"': g_string_append_c(out, '"'); break;
		case '\\': g_string_append_c(out, '\\'); break;
		case '/': g_string_append_c(out, '/'); break;
		case 'u':
			if (i + 4 < len) {
				gchar hex[5] = { s[i + 1], s[i + 2], s[i + 3], s[i + 4], 0 };
				gunichar cp = (gunichar) g_ascii_strtoull(hex, NULL, 16);
				g_string_append_unichar(out, cp);
				i += 4;
			}
			break;
		default:
			g_string_append_c(out, s[i]);
		}
	}
	return g_string_free(out, FALSE);
}

static gchar *json_get_string(const gchar *json, const gchar *key) {
	gchar *needle = g_strdup_printf("\"%s\"", key);
	const gchar *p = strstr(json, needle);
	g_free(needle);
	if (p == NULL)
		return NULL;
	p += strlen(key) + 2;
	while (*p != '\0' && g_ascii_isspace(*p))
		p++;
	if (*p != ':')
		return NULL;
	p++;
	while (*p != '\0' && g_ascii_isspace(*p))
		p++;
	if (*p != '"')
		return NULL;
	p++;
	const gchar *start = p;
	while (*p != '\0' && *p != '"') {
		if (*p == '\\' && p[1] != '\0')
			p++;
		p++;
	}
	if (*p != '"')
		return NULL;
	return json_unescape(start, (gsize) (p - start));
}

static gchar *escape_json(const gchar *s) {
	GString *out = g_string_new(NULL);
	if (s == NULL)
		return g_string_free(out, FALSE);
	for (const unsigned char *p = (const unsigned char *) s; *p; p++) {
		switch (*p) {
		case '"': g_string_append(out, "\\\""); break;
		case '\\': g_string_append(out, "\\\\"); break;
		case '\n': g_string_append(out, "\\n"); break;
		case '\r': g_string_append(out, "\\r"); break;
		case '\t': g_string_append(out, "\\t"); break;
		default:
			if (*p < 0x20)
				g_string_append_printf(out, "\\u%04x", *p);
			else
				g_string_append_c(out, (char) *p);
		}
	}
	return g_string_free(out, FALSE);
}

/* ------------------------------------------------------------------ */
/* Transport                                                           */
/* ------------------------------------------------------------------ */

static void send_all(int fd, const gchar *text) {
	size_t remaining = strlen(text);
	const gchar *cursor = text;
	while (remaining > 0) {
		ssize_t written = send(fd, cursor, remaining, 0);
		if (written <= 0)
			return;
		cursor += written;
		remaining -= (size_t) written;
	}
}

static void reply_raw(ClientCtx *ctx, const gchar *id, const gchar *payload) {
	gchar *escaped_id = escape_json(id ? id : "");
	GString *line = g_string_new(NULL);
	g_string_append_printf(line, "{\"id\":\"%s\",%s\n", escaped_id,
			payload[0] == '{' ? payload + 1 : payload);
	send_all(ctx->fd, line->str);
	g_string_free(line, TRUE);
	g_free(escaped_id);
}

static void reply_error(ClientCtx *ctx, const gchar *id, const gchar *message) {
	gchar *escaped = escape_json(message);
	gchar *escaped_id = escape_json(id ? id : "");
	gchar *line = g_strdup_printf("{\"id\":\"%s\",\"ok\":false,\"error\":\"%s\"}\n",
			escaped_id, escaped);
	send_all(ctx->fd, line);
	g_free(line);
	g_free(escaped);
	g_free(escaped_id);
}

static gboolean token_ok(const gchar *request) {
	if (server_token == NULL || *server_token == '\0')
		return TRUE;
	gchar *given = json_get_string(request, "token");
	gboolean ok = (given != NULL && g_strcmp0(given, server_token) == 0);
	g_free(given);
	return ok;
}

static void handle_request(ClientCtx *ctx, const gchar *line) {
	if (line == NULL || *line == '\0')
		return;

	if (line[0] != '{') {
		/* raw command mode */
		gchar *reply = siril_ohos_exec(line);
		reply_raw(ctx, NULL, reply);
		g_free(reply);
		return;
	}

	gchar *id = json_get_string(line, "id");
	if (!token_ok(line)) {
		reply_error(ctx, id, "invalid token");
		g_free(id);
		return;
	}

	gchar *op = json_get_string(line, "op");
	if (op == NULL)
		op = g_strdup("exec");

	if (g_strcmp0(op, "ping") == 0) {
		reply_raw(ctx, id, "{\"ok\":true,\"pong\":true}");
	} else if (g_strcmp0(op, "status") == 0) {
		gchar *status = siril_ohos_status();
		gchar *reply = g_strdup_printf("{\"ok\":true,\"status\":%s}", status);
		reply_raw(ctx, id, reply);
		g_free(reply);
		g_free(status);
	} else if (g_strcmp0(op, "version") == 0) {
		gchar *escaped = escape_json(siril_ohos_version());
		gchar *reply = g_strdup_printf("{\"ok\":true,\"version\":\"%s\"}", escaped);
		reply_raw(ctx, id, reply);
		g_free(reply);
		g_free(escaped);
	} else if (g_strcmp0(op, "catalog") == 0) {
		gchar *catalog = siril_ohos_catalog();
		gchar *reply = g_strdup_printf("{\"ok\":true,\"commands\":%s}", catalog);
		reply_raw(ctx, id, reply);
		g_free(reply);
		g_free(catalog);
	} else if (g_strcmp0(op, "chdir") == 0) {
		gchar *path = json_get_string(line, "path");
		int rc = siril_ohos_chdir(path);
		gchar *status = siril_ohos_status();
		gchar *reply = g_strdup_printf("{\"ok\":%s,\"status\":%s}",
				rc == 0 ? "true" : "false", status);
		reply_raw(ctx, id, reply);
		g_free(reply);
		g_free(status);
		g_free(path);
	} else if (g_strcmp0(op, "exec") == 0) {
		gchar *cmd = json_get_string(line, "cmd");
		if (cmd == NULL) {
			reply_error(ctx, id, "missing cmd");
		} else {
			gchar *reply = siril_ohos_exec(cmd);
			reply_raw(ctx, id, reply);
			g_free(reply);
			g_free(cmd);
		}
	} else {
		reply_error(ctx, id, "unknown op");
	}

	g_free(op);
	g_free(id);
}

static gpointer client_thread(gpointer data) {
	ClientCtx *ctx = (ClientCtx *) data;
	gchar *buffer = g_malloc0(MAX_REQUEST);
	size_t used = 0;

	struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
	setsockopt(ctx->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	while (TRUE) {
		g_mutex_lock(&server_mutex);
		gboolean running = server_running;
		g_mutex_unlock(&server_mutex);
		if (!running)
			break;

		ssize_t got = recv(ctx->fd, buffer + used, MAX_REQUEST - used - 1, 0);
		if (got == 0)
			break;
		if (got < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
				continue;
			break;
		}
		used += (size_t) got;
		buffer[used] = '\0';

		gchar *start = buffer;
		gchar *newline = NULL;
		while ((newline = strchr(start, '\n')) != NULL) {
			*newline = '\0';
			g_strchomp(start);
			handle_request(ctx, start);
			start = newline + 1;
		}
		size_t remaining = strlen(start);
		if (remaining > 0 && start != buffer)
			memmove(buffer, start, remaining + 1);
		used = remaining;
		if (used >= MAX_REQUEST - 1)
			break;
	}

	g_free(buffer);
	close(ctx->fd);
	g_mutex_lock(&server_mutex);
	server_clients--;
	g_mutex_unlock(&server_mutex);
	g_free(ctx->peer);
	g_free(ctx);
	return NULL;
}

static gpointer accept_main(gpointer data) {
	(void) data;
	while (TRUE) {
		g_mutex_lock(&server_mutex);
		gboolean running = server_running;
		int fd = listen_fd;
		g_mutex_unlock(&server_mutex);
		if (!running || fd < 0)
			break;

		struct sockaddr_in peer;
		socklen_t peer_len = sizeof(peer);
		int client = accept(fd, (struct sockaddr *) &peer, &peer_len);
		if (client < 0) {
			if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
				g_usleep(50 * 1000);
				continue;
			}
			break;
		}

		ClientCtx *ctx = g_new0(ClientCtx, 1);
		ctx->fd = client;
		ctx->peer = g_strdup_printf("%s:%d", inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));

		g_mutex_lock(&server_mutex);
		server_clients++;
		client_seq++;
		g_mutex_unlock(&server_mutex);

		OHOS_LOG_INFO("server: client %{public}s", ctx->peer);
		g_thread_unref(g_thread_new("siril-client", client_thread, ctx));
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int siril_ohos_server_start(const gchar *bind_addr, int port, const gchar *token) {
	g_mutex_lock(&server_mutex);
	if (server_running) {
		g_mutex_unlock(&server_mutex);
		return -1;
	}
	if (port <= 0 || port > 65535) {
		g_mutex_unlock(&server_mutex);
		return -2;
	}
	g_mutex_unlock(&server_mutex);

	gchar *bind_copy = g_strdup(bind_addr && *bind_addr ? bind_addr : "127.0.0.1");

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		g_free(bind_copy);
		return -4;
	}

	int reuse = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t) port);
	if (inet_pton(AF_INET, bind_copy, &addr.sin_addr) != 1)
		inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

	if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
		OHOS_LOG_ERROR("server: bind %{public}s:%{public}d failed errno=%{public}d", bind_copy, port, errno);
		close(fd);
		g_free(bind_copy);
		return -3;
	}
	if (listen(fd, 16) != 0) {
		close(fd);
		g_free(bind_copy);
		return -3;
	}

	g_mutex_lock(&server_mutex);
	g_free(server_bind);
	server_bind = bind_copy;
	g_free(server_token);
	server_token = g_strdup(token ? token : "");
	server_port = port;
	server_clients = 0;
	listen_fd = fd;
	server_running = TRUE;
	accept_thread = g_thread_new("siril-accept", accept_main, NULL);
	g_mutex_unlock(&server_mutex);
	OHOS_LOG_INFO("server: listening on %{public}s:%{public}d", server_bind, server_port);
	return 0;
}

int siril_ohos_server_stop(void) {
	g_mutex_lock(&server_mutex);
	if (!server_running) {
		g_mutex_unlock(&server_mutex);
		return 0;
	}
	server_running = FALSE;
	int fd = listen_fd;
	listen_fd = -1;
	GThread *thread = accept_thread;
	accept_thread = NULL;
	g_mutex_unlock(&server_mutex);

	if (fd >= 0) {
		shutdown(fd, SHUT_RDWR);
		close(fd);
	}
	if (thread) {
		g_thread_join(thread);
	}
	/* client threads notice the flag within their 1s receive timeout */
	return 0;
}

char *siril_ohos_server_status(void) {
	g_mutex_lock(&server_mutex);
	gchar *line = g_strdup_printf(
			"{\"running\":%s,\"bind\":\"%s\",\"port\":%d,\"clients\":%d,\"requests\":%d}",
			server_running ? "true" : "false",
			server_bind ? server_bind : "127.0.0.1",
			server_port, server_clients, client_seq);
	g_mutex_unlock(&server_mutex);
	return line;
}
