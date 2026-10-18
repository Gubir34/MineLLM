#include "settings.h"

#include <json-glib/json-glib.h>
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

static void wipe(char *s)
{
    if (s) {
        memset(s, 0, strlen(s));
        g_free(s);
    }
}

/* Profiles before 0.2 were the API key alone (kind "api_key") plus model and
 * base_url in the file. */
static char *legacy_key(const char *provider)
{
    return secret_get("api_key", provider);
}

/* Profiles the keyring refused, for this session (provider -> JSON). */
static GHashTable *session_profiles;

gboolean settings_load_profile(const char *provider, char **model, char **base_url, char **api_key)
{
    *model = *base_url = *api_key = NULL;
    char *raw = secret_get("provider", provider);
    if (!raw && session_profiles && g_hash_table_contains(session_profiles, provider))
        raw = g_strdup(g_hash_table_lookup(session_profiles, provider));
    JsonNode *n = raw ? json_from_string(raw, NULL) : NULL;
    wipe(raw);
    gboolean found = n && JSON_NODE_HOLDS_OBJECT(n);
    if (found) {
        JsonObject *o = json_node_get_object(n);
        *model = g_strdup(json_object_get_string_member_with_default(o, "model", ""));
        *base_url = g_strdup(json_object_get_string_member_with_default(o, "base_url", ""));
        *api_key = g_strdup(json_object_get_string_member_with_default(o, "api_key", ""));
        /* the parsed copy of the key is wiped with the tree */
        JsonNode *k = json_object_get_member(o, "api_key");
        if (k && JSON_NODE_HOLDS_VALUE(k)) {
            const char *kv = json_node_get_string(k);
            if (kv)
                memset((char *)kv, 0, strlen(kv));
        }
    } else {
        *api_key = legacy_key(provider);
        found = *api_key != NULL;
    }
    if (n)
        json_node_unref(n);
    if (!*model)
        *model = g_strdup("");
    if (!*base_url)
        *base_url = g_strdup("");
    if (!*api_key)
        *api_key = g_strdup("");
    return found;
}

gboolean settings_save_profile(const char *provider, const char *model, const char *base_url, const char *api_key,
                               char **why)
{
    GError *err = NULL;
    gboolean ok;
    if (!*model && !*base_url && !*api_key) {
        ok = secret_put("provider", provider, NULL, NULL, &err);
    } else {
        JsonObject *o = json_object_new();
        json_object_set_string_member(o, "model", model);
        json_object_set_string_member(o, "base_url", base_url);
        json_object_set_string_member(o, "api_key", api_key);
        JsonNode *n = json_node_init_object(json_node_alloc(), o);
        char *raw = json_to_string(n, FALSE);
        char *label = g_strdup_printf("MineLLM model profile: %s", provider);
        ok = secret_put("provider", provider, label, raw, &err);
        g_free(label);
        if (!ok) {
            if (!session_profiles)
                session_profiles = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)wipe);
            g_hash_table_replace(session_profiles, g_strdup(provider), g_strdup(raw));
        } else if (session_profiles) {
            g_hash_table_remove(session_profiles, provider);
        }
        wipe(raw);
        JsonNode *k = json_object_get_member(o, "api_key");
        memset((char *)json_node_get_string(k), 0, strlen(api_key));
        json_node_unref(n);
        json_object_unref(o);
    }
    if (ok) {
        GError *e2 = NULL; /* the old key-only entry is replaced by the profile */
        secret_password_clear_sync(schema(), NULL, &e2, "kind", "api_key", "key", provider, NULL);
        g_clear_error(&e2);
        return TRUE;
    }
    if (why)
        *why = g_strdup_printf("the keyring is not available (%s): the model settings are kept until MineLLM closes",
                               err ? err->message : "unknown error");
    g_clear_error(&err);
    return FALSE;
}

static void save_choice(const Settings *s);

void settings_use_provider(Settings *s, const char *provider)
{
    if (g_strcmp0(s->provider, provider)) {
        g_free(s->provider);
        s->provider = g_strdup(provider);
    }
    g_free(s->model);
    g_free(s->base_url);
    wipe(s->api_key);
    settings_load_profile(provider, &s->model, &s->base_url, &s->api_key);
    save_choice(s);
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

static void replace_str(char **dst, const char *src)
{
    g_free(*dst);
    *dst = g_strdup(src);
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
    char *old_model = str(kf, "model", "model", ""), *old_url = str(kf, "model", "base_url", "");
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
    gboolean have = settings_load_profile(s->provider, &s->model, &s->base_url, &s->api_key);
    if (*old_model || *old_url) {
        /* moving an older file's model and address into the encrypted profile */
        if (!*s->model)
            replace_str(&s->model, old_model);
        if (!*s->base_url)
            replace_str(&s->base_url, old_url);
        if (settings_save_profile(s->provider, s->model, s->base_url, s->api_key, NULL))
            save_choice(s);
    }
    (void)have;
    g_free(old_model);
    g_free(old_url);
    return s;
}

static gboolean write_file(const Settings *s, char **why)
{
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_string(kf, "server", "host", s->host);
    g_key_file_set_integer(kf, "server", "port", s->port);
    g_key_file_set_string(kf, "server", "username", s->username);
    g_key_file_set_string(kf, "model", "provider", s->provider);
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
        if (why)
            *why = g_strdup_printf("cannot save the settings: %s", err->message);
        g_error_free(err);
    }
    return ok;
}

static void save_choice(const Settings *s)
{
    write_file(s, NULL);
}

gboolean settings_save(const Settings *s, char **why)
{
    if (!write_file(s, why))
        return FALSE;
    char *acct = account_key(s);
    char *label = g_strdup_printf("MineLLM login password for %s", acct);
    GError *err = NULL;
    gboolean ok = secret_put("login_password", acct, label, s->password, &err);
    g_free(acct);
    g_free(label);
    if (!ok) {
        *why = g_strdup_printf("the keyring is not available (%s): the password and API key are kept until MineLLM closes",
                               err ? err->message : "unknown error");
        g_clear_error(&err);
        return FALSE;
    }
    return settings_save_profile(s->provider, s->model, s->base_url, s->api_key, why);
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
