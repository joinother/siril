/*
 * NAPI bindings for the embedded SIRIL engine.
 *
 * The heavy lifting lives in libsiril-ohos.so (built by meson from the SIRIL
 * tree); this module only marshals strings and keeps the ArkTS side simple.
 *
 * Copyright (C) 2026 team free-astro (see more in AUTHORS file)
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <napi/native_api.h>

#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {
int siril_ohos_init(const char *files_dir, const char *work_dir, const char *catalog);
const char *siril_ohos_version(void);
char *siril_ohos_exec(const char *command);
char *siril_ohos_status(void);
char *siril_ohos_catalog(void);
int siril_ohos_chdir(const char *dir);
int siril_ohos_server_start(const char *bind_addr, int port, const char *token);
int siril_ohos_server_stop(void);
char *siril_ohos_server_status(void);
void siril_ohos_free(char *text);
void siril_ohos_shutdown(void);
}

namespace {

std::string GetString(napi_env env, napi_value value) {
    size_t length = 0;
    if (napi_get_value_string_utf8(env, value, nullptr, 0, &length) != napi_ok) {
        return std::string();
    }
    std::string out(length, '\0');
    size_t copied = 0;
    napi_get_value_string_utf8(env, value, out.data(), length + 1, &copied);
    out.resize(copied);
    return out;
}

napi_value MakeString(napi_env env, const char *text) {
    napi_value result = nullptr;
    napi_create_string_utf8(env, text ? text : "", NAPI_AUTO_LENGTH, &result);
    return result;
}

napi_value MakeInt(napi_env env, int value) {
    napi_value result = nullptr;
    napi_create_int32(env, value, &result);
    return result;
}

napi_value InitEngine(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    std::string files_dir = argc > 0 ? GetString(env, argv[0]) : "";
    std::string work_dir = argc > 1 ? GetString(env, argv[1]) : "";
    std::string catalog = argc > 2 ? GetString(env, argv[2]) : "";

    int rc = siril_ohos_init(files_dir.c_str(), work_dir.c_str(), catalog.c_str());
    return MakeInt(env, rc);
}

napi_value Version(napi_env env, napi_callback_info /*info*/) {
    return MakeString(env, siril_ohos_version());
}

napi_value Status(napi_env env, napi_callback_info /*info*/) {
    char *json = siril_ohos_status();
    napi_value result = MakeString(env, json);
    siril_ohos_free(json);
    return result;
}

napi_value Catalog(napi_env env, napi_callback_info /*info*/) {
    char *json = siril_ohos_catalog();
    napi_value result = MakeString(env, json);
    siril_ohos_free(json);
    return result;
}

napi_value Chdir(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    std::string dir = argc > 0 ? GetString(env, argv[0]) : "";
    return MakeInt(env, siril_ohos_chdir(dir.c_str()));
}

struct ExecWork {
    napi_env env;
    napi_async_work work;
    napi_deferred deferred;
    std::string command;
    char *result;
    bool ok;
};

void ExecuteCallback(napi_env /*env*/, void *data) {
    ExecWork *w = static_cast<ExecWork *>(data);
    w->result = siril_ohos_exec(w->command.c_str());
    w->ok = w->result != nullptr;
}

void CompleteCallback(napi_env env, napi_status status, void *data) {
    ExecWork *w = static_cast<ExecWork *>(data);
    napi_value value = nullptr;
    if (status == napi_ok && w->ok) {
        napi_create_string_utf8(env, w->result, NAPI_AUTO_LENGTH, &value);
        napi_resolve_deferred(env, w->deferred, value);
    } else {
        napi_value message = MakeString(env, "command execution failed");
        napi_create_error(env, nullptr, message, &value);
        napi_reject_deferred(env, w->deferred, value);
    }
    if (w->result) {
        siril_ohos_free(w->result);
    }
    napi_delete_async_work(env, w->work);
    delete w;
}

napi_value RunCommand(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    napi_value promise = nullptr;
    napi_deferred deferred = nullptr;
    napi_create_promise(env, &deferred, &promise);

    ExecWork *w = new ExecWork();
    w->env = env;
    w->deferred = deferred;
    w->command = argc > 0 ? GetString(env, argv[0]) : "";
    w->result = nullptr;
    w->ok = false;

    napi_value resource_name = MakeString(env, "siril_exec");
    napi_create_async_work(env, nullptr, resource_name, ExecuteCallback, CompleteCallback, w, &w->work);
    napi_queue_async_work(env, w->work);
    return promise;
}

napi_value StartServer(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    std::string bind = argc > 0 ? GetString(env, argv[0]) : "127.0.0.1";
    int32_t port = 0;
    if (argc > 1) {
        napi_get_value_int32(env, argv[1], &port);
    }
    std::string token = argc > 2 ? GetString(env, argv[2]) : "";
    return MakeInt(env, siril_ohos_server_start(bind.c_str(), port, token.c_str()));
}

napi_value StopServer(napi_env env, napi_callback_info /*info*/) {
    return MakeInt(env, siril_ohos_server_stop());
}

napi_value ServerStatus(napi_env env, napi_callback_info /*info*/) {
    char *json = siril_ohos_server_status();
    napi_value result = MakeString(env, json);
    siril_ohos_free(json);
    return result;
}

napi_value Shutdown(napi_env /*env*/, napi_callback_info /*info*/) {
    siril_ohos_server_stop();
    siril_ohos_shutdown();
    return nullptr;
}

napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        {"initEngine", nullptr, InitEngine, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"version", nullptr, Version, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runCommand", nullptr, RunCommand, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"status", nullptr, Status, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"catalog", nullptr, Catalog, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"chdir", nullptr, Chdir, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"startServer", nullptr, StartServer, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stopServer", nullptr, StopServer, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"serverStatus", nullptr, ServerStatus, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"shutdown", nullptr, Shutdown, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}

napi_module g_sirilModule = {
    1,
    0,
    nullptr,
    Init,
    "siril",
    nullptr,
    {0},
};

}  // namespace

extern "C" __attribute__((constructor)) void RegisterSirilModule(void) {
    napi_module_register(&g_sirilModule);
}
