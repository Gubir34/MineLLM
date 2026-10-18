#include "settings.h"

#include <libsecret/secret.h>
#include <string.h>

static const SecretSchema *schema(void)
{
    static const SecretSchema s = {
        .name = "io.github.gubir34.MineLLM",
        .flags = SECRET_SCHEMA_NONE,
        .attributes = {{"kind", SECRET_SCHEMA_ATTRIBUTE_STRING}, {"key", SECRET_SCHEMA_ATTRIBUTE_STRING}, {NULL, 0}},
    };
    return &s;
}

static char *config_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "minellm", "settings.ini", NULL);
}

static char *secret_get(const char *kind, const char *key)
{
    GError *err = NULL;
    char *v = secret_password_lookup_sync(schema(), NULL, &err, "kind", kind, "key", key, NULL);
    if (err) {
        g_warning("keyring lookup failed: %s", err->message);
        g_error_free(err);
    }
    return v;
}

static gboolean secret_put(const char *kind, const char *key, const char *label, const char *value, GError **err)
{
    if (!value || !*value)
        return secret_password_clear_sync(schema(), NULL, err, "kind", kind, "key", key, NULL) || !*err;
    return secret_password_store_sync(schema(), SECRET_COLLECTION_DEFAULT, label, value, NULL, err, "kind", kind, "key", key,
                                      NULL);
}

static char *account_key(const Settings *s)
{
    return g_strdup_printf("%s@%s:%d", s->username, s->host, s->port);
}

char *settings_lookup_api_key(const char *provider)
{
    char *v = secret_get("api_key", provider);
    return v ? v : g_strdup("");
}

static char *str(GKeyFile *kf, const char *group, const char *key, const char *def)
{
    char *v = g_key_file_get_string(kf, group, key, NULL);
    return v ? v : g_strdup(def);
}

static int num(GKeyFile *kf, const char *group, const char *key, int def)
{
    GError *err = NULL;
    int v = g_key_file_get_integer(kf, group, key, &err);
    if (err) {
        g_error_free(err);
        return def;
    }
    return v;
}

Settings *settings_load(void)
{
    Settings *s = g_new0(Settings, 1);
    GKeyFile *kf = g_key_file_new();
    char *path = config_path();
    g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);
    g_free(path);
    s->host = str(kf, "server", "host", "127.0.0.1");
    s->port = num(kf, "server", "port", 25565);
    s->username = str(kf, "server", "username", "MineLLM");
    s->provider = str(kf, "model", "provider", "ollama");
    s->model = str(kf, "model", "model", "");
    s->base_url = str(kf, "model", "base_url", "");
    s->owners = str(kf, "agent", "owners", "");
    s->listen_chat = num(kf, "agent", "listen_chat", 1) != 0;
    s->max_steps = CLAMP(num(kf, "agent", "max_steps", 40), 5, 500);
    g_key_file_unref(kf);
    char *acct = account_key(s);
    s->password = secret_get("login_password", acct);
    if (!s->password || !*s->password) {
        /* as in MineMCP: the environment can supply it; it is not kept there */
        g_free(s->password);
        s->password = g_strdup(g_getenv("MC_LOGIN_PASSWORD") ? g_getenv("MC_LOGIN_PASSWORD") : "");
    }
    g_unsetenv("MC_LOGIN_PASSWORD");
    g_free(acct);
    s->api_key = settings_lookup_api_key(s->provider);
    return s;
}

gboolean settings_save(const Settings *s, char **why)
{
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_string(kf, "server", "host", s->host);
    g_key_file_set_integer(kf, "server", "port", s->port);
    g_key_file_set_string(kf, "server", "username", s->username);
    g_key_file_set_string(kf, "model", "provider", s->provider);
    g_key_file_set_string(kf, "model", "model", s->model);
    g_key_file_set_string(kf, "model", "base_url", s->base_url);
    g_key_file_set_string(kf, "agent", "owners", s->owners);
    g_key_file_set_integer(kf, "agent", "listen_chat", s->listen_chat);
    g_key_file_set_integer(kf, "agent", "max_steps", s->max_steps);
    char *path = config_path();
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    GError *err = NULL;
    gboolean ok = g_key_file_save_to_file(kf, path, &err);
    g_free(path);
    g_key_file_unref(kf);
    if (!ok) {
        *why = g_strdup_printf("cannot save the settings: %s", err->message);
        g_error_free(err);
        return FALSE;
    }
    char *acct = account_key(s);
    char *label1 = g_strdup_printf("MineLLM login password for %s", acct);
    char *label2 = g_strdup_printf("MineLLM API key for %s", s->provider);
    ok = secret_put("login_password", acct, label1, s->password, &err) &&
         secret_put("api_key", s->provider, label2, s->api_key, &err);
    g_free(acct);
    g_free(label1);
    g_free(label2);
    if (!ok) {
        *why = g_strdup_printf("the keyring is not available (%s): the password and API key are kept until MineLLM closes",
                               err ? err->message : "unknown error");
        g_clear_error(&err);
        return FALSE;
    }
    return TRUE;
}

void settings_free(Settings *s)
{
    if (!s)
        return;
    g_free(s->host);
    g_free(s->username);
    if (s->password)
        memset(s->password, 0, strlen(s->password));
    g_free(s->password);
    g_free(s->provider);
    g_free(s->model);
    if (s->api_key)
        memset(s->api_key, 0, strlen(s->api_key));
    g_free(s->api_key);
    g_free(s->base_url);
    g_free(s->owners);
    g_free(s);
}

char **settings_owner_list(const Settings *s)
{
    char **parts = g_strsplit_set(s->owners ? s->owners : "", ",; ", -1);
    GPtrArray *out = g_ptr_array_new();
    for (char **p = parts; *p; p++)
        if (**p)
            g_ptr_array_add(out, g_ascii_strdown(*p, -1));
    g_ptr_array_add(out, NULL);
    g_strfreev(parts);
    return (char **)g_ptr_array_free(out, FALSE);
}
