/*
 * SIRIL HarmonyOS bridge — embedded engine lifecycle and command execution.
 *
 * The code below mirrors the headless start-up path of src/main-cli.c
 * (siril_app_activate) without ever touching GApplication, GTK or the python
 * venv.  A single GMutex serialises command execution so the NAPI thread and
 * the TCP control server can share one engine instance safely.
 *
 * Copyright (C) 2026 team free-astro (see more in AUTHORS file)
 *
 * Siril is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <gsl/gsl_errno.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "core/command.h"
#include "core/command_line_processor.h"
#include "core/icc_profile.h"
#include "core/initfile.h"
#include "core/OS_utils.h"
#include "core/processing_thread.h"
#include "core/proto.h"
#include "core/settings.h"
#include "core/siril.h"
#include "core/siril_app_dirs.h"
#include "core/siril_language.h"
#include "core/siril_log.h"
#include "algos/siril_random.h"
#include "io/conversion.h"
#include "core/siril_update.h"

#if defined(__OHOS__)
#include <hilog/log.h>
#define OHOS_LOG_INFO(...) OH_LOG_Print(LOG_APP, LOG_INFO, 0x5349, "SIRIL", __VA_ARGS__)
#define OHOS_LOG_ERROR(...) OH_LOG_Print(LOG_APP, LOG_ERROR, 0x5349, "SIRIL", __VA_ARGS__)
#else
#define OHOS_LOG_INFO(...) g_print(__VA_ARGS__)
#define OHOS_LOG_ERROR(...) g_printerr(__VA_ARGS__)
#endif
#include "io/sequence.h"
#include "ohos/siril_ohos.h"

/* globals normally provided by main-cli.c / main.c */
cominfo com;
fits *gfit = NULL;

static gboolean engine_ready = FALSE;
static GMutex exec_mutex;
static gchar *catalog_path = NULL;
static gchar *catalog_json = NULL;

/* ------------------------------------------------------------------ */
/* JSON helpers (flat, string oriented — enough for the control API)   */
/* ------------------------------------------------------------------ */

static void json_escape_append(GString *out, const char *s) {
	if (s == NULL)
		return;
	for (const unsigned char *p = (const unsigned char *) s; *p; p++) {
		switch (*p) {
		case '"':  g_string_append(out, "\\\""); break;
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
}

static void json_string_field(GString *out, const char *key, const char *value, gboolean comma) {
	g_string_append_printf(out, "%s\"%s\":\"", comma ? "," : "", key);
	json_escape_append(out, value ? value : "");
	g_string_append(out, "\"");
}

/* ------------------------------------------------------------------ */
/* Engine start-up                                                     */
/* ------------------------------------------------------------------ */

static void engine_global_initialization(void) {
	gsl_set_error_handler_off();
	com.star_is_seqdata = FALSE;
	com.stars = NULL;
	com.tilt = NULL;
	com.uniq = NULL;
	com.kernel = NULL;
	com.kernelsize = 0;
	com.kernelchannels = 0;
	memset(&com.selection, 0, sizeof(rectangle));
	memset(&com.layers_hist, 0, sizeof(com.layers_hist));
	initialize_default_settings();

	siril_log_debug("Initializing processing thread...\n");
	processing_system_init();

#ifdef HAVE_FFTW3F_OMP
	fftwf_init_threads();
#endif
#ifdef _OPENMP
	omp_set_max_active_levels(2);
#endif
}

static void ensure_sandbox_env(void) {
	/* The application passes real paths through the environment; make sure a
	 * few GLib lookups never escape to a read-only system location. */
	const char *home = g_getenv("HOME");
	if (home && *home) {
		gchar *cfg = g_build_filename(home, "config", NULL);
		gchar *data = g_build_filename(home, "data", NULL);
		gchar *cache = g_build_filename(home, "cache", NULL);
		gchar *tmp = g_build_filename(home, "tmp", NULL);
		g_mkdir_with_parents(cfg, 0755);
		g_mkdir_with_parents(data, 0755);
		g_mkdir_with_parents(cache, 0755);
		g_mkdir_with_parents(tmp, 0755);
		if (!g_getenv("XDG_CONFIG_HOME"))
			g_setenv("XDG_CONFIG_HOME", cfg, TRUE);
		if (!g_getenv("XDG_DATA_HOME"))
			g_setenv("XDG_DATA_HOME", data, TRUE);
		if (!g_getenv("XDG_CACHE_HOME"))
			g_setenv("XDG_CACHE_HOME", cache, TRUE);
		if (!g_getenv("TMPDIR"))
			g_setenv("TMPDIR", tmp, TRUE);
		g_free(cfg);
		g_free(data);
		g_free(cache);
		g_free(tmp);
	}
}


/* Collapse whitespace outside strings so the catalog can be embedded in one
 * protocol line. */
static gchar *json_minify(const gchar *input) {
	GString *out = g_string_new(NULL);
	gboolean in_string = FALSE, escaped = FALSE;
	for (const gchar *p = input; *p; p++) {
		gchar c = *p;
		if (in_string) {
			g_string_append_c(out, c);
			if (escaped)
				escaped = FALSE;
			else if (c == '\\')
				escaped = TRUE;
			else if (c == '"')
				in_string = FALSE;
			continue;
		}
		if (c == '"') {
			in_string = TRUE;
			g_string_append_c(out, c);
		} else if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
			continue;
		} else {
			g_string_append_c(out, c);
		}
	}
	return g_string_free(out, FALSE);
}

int siril_ohos_init(const char *files_dir, const char *work_dir, const char *catalog) {
	if (engine_ready)
		return 0;

	if (files_dir && *files_dir) {
		g_mkdir_with_parents(files_dir, 0755);
		g_setenv("HOME", files_dir, TRUE);
		if (!g_getenv("APPDIR"))
			g_setenv("APPDIR", files_dir, TRUE);
	}
	ensure_sandbox_env();

	/* main-cli.c allocates gfit before anything else; read_single_image()
	 * writes into it, so a NULL here would crash the first `load`. */
	if (gfit == NULL)
		gfit = calloc(1, sizeof(fits));
	initialize_siril_directories();

	OHOS_LOG_INFO("init: files=%{public}s work=%{public}s", files_dir ? files_dir : "(null)", work_dir ? work_dir : "(null)");
	setlocale(LC_NUMERIC, "C");
	com.script = TRUE;
	com.headless = TRUE;
	siril_initialize_rng();
	engine_global_initialization();

	com.spcc_remote_catalogue = g_strdup("https://zenodo.org/records/17988559/files");
	com.spcc_remote_catalogue_xpcts = NULL;

	initialize_sequence(&com.seq, TRUE);

	if (checkinitfile()) {
		siril_log_message("Could not load or create settings file\n");
		OHOS_LOG_ERROR("init: checkinitfile failed");
		return -2;
	}
	if (com.pref.lang)
		language_init(com.pref.lang);

	gchar *version_string = get_siril_version_string();
	siril_log_message("Welcome to %s - HarmonyOS\n", version_string);
	g_free(version_string);

	gchar *supported_files = initialize_converters();
	siril_log_message("Supported file types: %s\n", supported_files);
	g_free(supported_files);

	if (work_dir && *work_dir) {
		gchar *err = NULL;
		siril_change_dir(work_dir, &err);
		g_free(err);
		if (com.pref.wd)
			g_free(com.pref.wd);
		com.pref.wd = g_strdup(work_dir);
	}

	init_num_procs();
	log_num_procs();
	initialize_profiles_and_transforms();

	if (catalog && *catalog) {
		catalog_path = g_strdup(catalog);
		gchar *content = NULL;
		if (g_file_get_contents(catalog, &content, NULL, NULL) && content) {
			g_strstrip(content);
			if (content[0] == '[')
				catalog_json = json_minify(content);
			g_free(content);
		}
	}

	engine_ready = TRUE;
	OHOS_LOG_INFO("init: engine ready, version=%{public}s", PACKAGE_VERSION);
	return 0;
}


#ifndef HAVE_LIBCURL
/* Built without libcurl: online catalogue refresh is unavailable.  SIRIL's
 * headless targets still reference this symbol, so provide an explicit no-op
 * instead of silently dropping the feature. */
void siril_check_spcc_mirrors(gboolean verbose, gboolean sync) {
	(void) sync;
	if (verbose)
		siril_log_message("Online SPCC catalogue is unavailable in this build (libcurl disabled).\n");
}
#endif

const char *siril_ohos_version(void) {
	return PACKAGE_VERSION;
}

/* ------------------------------------------------------------------ */
/* Execution                                                           */
/* ------------------------------------------------------------------ */

static int read_all_fd(int fd, GString *out) {
	char buf[8192];
	ssize_t n;
	lseek(fd, 0, SEEK_SET);
	while ((n = read(fd, buf, sizeof(buf))) > 0)
		g_string_append_len(out, buf, (gssize) n);
	return 0;
}

/* Removes the "input command:" echo and trailing blank lines so the returned
 * text is exactly what the command produced. */
static void tidy_output(GString *out) {
	gchar **lines = g_strsplit(out->str, "\n", -1);
	GString *clean = g_string_new(NULL);
	gboolean first = TRUE;
	for (int i = 0; lines[i]; i++) {
		if (first && g_str_has_prefix(lines[i], "input command:")) {
			first = FALSE;
			continue;
		}
		first = FALSE;
		/* siril_log writes "log: " prefixes when stdout is a terminal-like
		 * sink; keep them out of the machine readable payload. */
		const gchar *line = lines[i];
		if (g_str_has_prefix(line, "log: "))
			line += 5;
		g_string_append(clean, line);
		if (lines[i + 1])
			g_string_append_c(clean, '\n');
	}
	g_strfreev(lines);
	g_strchomp(clean->str);
	g_string_assign(out, clean->str);
	g_string_free(clean, TRUE);
}

char *siril_ohos_status(void) {
	GString *out = g_string_new("{\"ready\":");
	g_string_append(out, engine_ready ? "true" : "false");
	json_string_field(out, "version", PACKAGE_VERSION, TRUE);
	json_string_field(out, "cwd", com.wd ? com.wd : "", TRUE);
	const char *image = "";
	if (com.uniq && com.uniq->filename)
		image = com.uniq->filename;
	json_string_field(out, "image", image, TRUE);
	gboolean has_seq = (com.seq.seqname != NULL);
	g_string_append(out, ",\"sequence\":{");
	json_string_field(out, "name", has_seq ? com.seq.seqname : "", FALSE);
	g_string_append_printf(out, ",\"number\":%d", has_seq ? com.seq.number : 0);
	g_string_append(out, ",\"loaded\":");
	g_string_append(out, has_seq ? "true" : "false");
	g_string_append_c(out, '}');
	if (com.max_thread > 0)
		g_string_append_printf(out, ",\"threads\":%d", com.max_thread);
	g_string_append_c(out, '}');

	char *result = g_strdup(out->str);
	g_string_free(out, TRUE);
	return result;
}

char *siril_ohos_exec(const char *command) {
	gint64 start = g_get_monotonic_time();
	GString *payload = g_string_new("{");

	if (!engine_ready) {
		g_string_append(payload, "\"ok\":false,\"code\":-1,\"error\":\"engine not initialised\"");
		g_string_append(payload, ",\"output\":\"\"");
		g_string_append(payload, "}");
		return g_strdup(payload->str);
	}
	if (command == NULL || *command == '\0') {
		g_string_append(payload, "\"ok\":false,\"code\":-1,\"error\":\"empty command\"");
		g_string_append(payload, ",\"output\":\"\"}");
		return g_strdup(payload->str);
	}

	g_mutex_lock(&exec_mutex);

	fflush(stdout);
	fflush(stderr);
	int saved_out = dup(STDOUT_FILENO);
	int saved_err = dup(STDERR_FILENO);

	gchar *tmpdir = g_strdup(g_get_tmp_dir());
	gchar *tmpl = g_build_filename(tmpdir, "siril-cmd-XXXXXX", NULL);
	int fd = g_mkstemp(tmpl);
	GString *output = g_string_new(NULL);
	int rc = -1;
	if (fd >= 0) {
		dup2(fd, STDOUT_FILENO);
		dup2(fd, STDERR_FILENO);

		OHOS_LOG_INFO("exec: start %{public}s", command);
		rc = processcommand(command, TRUE);
		OHOS_LOG_INFO("exec: done rc=%{public}d", rc);

		fflush(stdout);
		fflush(stderr);
		read_all_fd(fd, output);
		close(fd);
		unlink(tmpl);
	}
	if (saved_out >= 0) {
		dup2(saved_out, STDOUT_FILENO);
		close(saved_out);
	}
	if (saved_err >= 0) {
		dup2(saved_err, STDERR_FILENO);
		close(saved_err);
	}
	g_free(tmpl);
	g_free(tmpdir);

	tidy_output(output);

	g_mutex_unlock(&exec_mutex);

	OHOS_LOG_INFO("exec: rc=%{public}d cmd=%{public}s", rc, command);
	g_string_append_printf(payload, "\"ok\":%s,\"code\":%d", rc == CMD_OK ? "true" : "false", rc);
	if (rc != CMD_OK)
		json_string_field(payload, "error", cmd_err_to_str(rc), TRUE);
	json_string_field(payload, "output", output->str, TRUE);
	g_string_append_printf(payload, ",\"ms\":%" G_GINT64_FORMAT, (g_get_monotonic_time() - start) / 1000);
	g_string_append(payload, ",\"status\":");
	char *status = siril_ohos_status();
	g_string_append(payload, status);
	g_free(status);
	g_string_append_c(payload, '}');

	g_string_free(output, TRUE);

	char *result = g_strdup(payload->str);
	g_string_free(payload, TRUE);
	return result;
}

char *siril_ohos_catalog(void) {
	if (catalog_json)
		return g_strdup(catalog_json);
	return g_strdup("[]");
}

int siril_ohos_chdir(const char *dir) {
	if (!engine_ready || dir == NULL || *dir == '\0')
		return -1;
	gchar *err = NULL;
	if (siril_change_dir(dir, &err)) {
		g_free(err);
		return -1;
	}
	g_free(err);
	if (com.pref.wd)
		g_free(com.pref.wd);
	com.pref.wd = g_strdup(dir);
	return 0;
}

void siril_ohos_free(char *text) {
	g_free(text);
}

void siril_ohos_shutdown(void) {
	if (!engine_ready)
		return;
	engine_ready = FALSE;
	g_free(catalog_path);
	catalog_path = NULL;
	g_free(catalog_json);
	catalog_json = NULL;
}
