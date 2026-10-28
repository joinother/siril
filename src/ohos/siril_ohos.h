/*
 * SIRIL HarmonyOS bridge — public C API
 *
 * This header is the only surface the NAPI module and the TCP control server
 * need.  It lets a HarmonyOS application embed the headless SIRIL engine
 * (the same code that powers siril-cli) inside a shared library and drive it
 * with the regular SIRIL command language.
 *
 * Copyright (C) 2026 team free-astro (see more in AUTHORS file)
 *
 * Siril is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef SIRIL_OHOS_H
#define SIRIL_OHOS_H

#include <glib.h>

G_BEGIN_DECLS

/* Initialise the embedded engine.
 *
 * files_dir  : writable application sandbox directory.  SIRIL keeps its
 *              config, cache and settings below this path.
 * work_dir   : initial working directory (where images are looked up).
 * catalog    : optional path to commands.json (generated at build time).
 *
 * Returns 0 on success, negative on failure.  Safe to call more than once;
 * only the first successful call performs the heavy initialisation.
 */
int siril_ohos_init(const char *files_dir, const char *work_dir, const char *catalog);

/* Engine version string, e.g. "1.5.0".  Never NULL. */
const char *siril_ohos_version(void);

/* Execute one SIRIL command line.  Blocks until the command (and its
 * background processing thread) finished.  Returns a newly allocated JSON
 * document, to be released with g_free():
 *
 *   {"ok":true,"code":0,"output":"...","ms":12,"status":{...}}
 */
char *siril_ohos_exec(const char *command);

/* JSON snapshot of the engine state (cwd, loaded image, sequence, ...). */
char *siril_ohos_status(void);

/* JSON array describing every command understood by the engine.  The data
 * comes from the generated catalog file; an empty array is returned when the
 * catalog was not shipped. */
char *siril_ohos_catalog(void);

/* Change the working directory.  Returns 0 on success. */
int siril_ohos_chdir(const char *dir);

/* TCP control server.  bind_addr may be NULL for 127.0.0.1, token may be NULL.
 * Returns 0 on success, negative errno-like value otherwise. */
int siril_ohos_server_start(const char *bind_addr, int port, const char *token);
int siril_ohos_server_stop(void);
char *siril_ohos_server_status(void);

/* Release a string returned by this API. */
void siril_ohos_free(char *text);

/* Release engine resources.  After this call the process should exit. */
void siril_ohos_shutdown(void);

G_END_DECLS

#endif /* SIRIL_OHOS_H */
