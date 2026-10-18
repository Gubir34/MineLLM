/* Test: per-provider model profiles in the keyring, and moving an older
 * settings.ini's model and address into one. Uses the real Secret Service
 * with the "groq" and "custom" providers and deletes their entries at the end;
 * run it with XDG_CONFIG_HOME pointing at an empty directory. */
#include "../src/settings.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void expect(const char *what, const char *got, const char *want)
{
    if (g_strcmp0(got, want)) {
        printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got, want);
        failures++;
    }
}

static char *ini(void)
{
    char *path = g_build_filename(g_get_user_config_dir(), "minellm", "settings.ini", NULL), *text = NULL;
    g_file_get_contents(path, &text, NULL, NULL);
    g_free(path);
    return text ? text : g_strdup("");
}

int main(void)
{
    char *m, *u, *k;
    if (settings_load_profile("groq", &m, &u, &k) || settings_load_profile("custom", &m, &u, &k)) {
        printf("groq/custom already have profiles in this keyring; not touching them\n");
        return 2;
    }
    char *why = NULL;
    if (!settings_save_profile("groq", "groq-model-1", "", "gsk_test_key", &why) ||
        !settings_save_profile("custom", "local-model", "http://127.0.0.1:1234/v1", "", &why)) {
        printf("keyring: %s\n", why);
        return 1;
    }
    settings_load_profile("groq", &m, &u, &k);
    expect("groq model", m, "groq-model-1");
    expect("groq key", k, "gsk_test_key");
    settings_load_profile("custom", &m, &u, &k);
    expect("custom url", u, "http://127.0.0.1:1234/v1");

    /* switching brings each profile back; the file keeps only the choice */
    Settings *s = settings_load();
    settings_use_provider(s, "groq");
    expect("switch model", s->model, "groq-model-1");
    expect("switch key", s->api_key, "gsk_test_key");
    settings_use_provider(s, "custom");
    expect("switch back url", s->base_url, "http://127.0.0.1:1234/v1");
    char *text = ini();
    if (strstr(text, "gsk_test_key") || strstr(text, "local-model") || strstr(text, "1234")) {
        printf("FAIL the file holds profile data:\n%s\n", text);
        failures++;
    }
    if (!strstr(text, "provider=custom")) {
        printf("FAIL the file does not remember the provider:\n%s\n", text);
        failures++;
    }
    g_free(text);
    settings_free(s);

    /* an older file: model and address in plain text, moved into the keyring */
    settings_save_profile("custom", "", "", "", NULL);
    char *path = g_build_filename(g_get_user_config_dir(), "minellm", "settings.ini", NULL);
    g_file_set_contents(path, "[model]\nprovider=custom\nmodel=old-model\nbase_url=http://old:1/v1\n", -1, NULL);
    s = settings_load();
    expect("migrated model", s->model, "old-model");
    settings_free(s);
    settings_load_profile("custom", &m, &u, &k);
    expect("migrated profile model", m, "old-model");
    expect("migrated profile url", u, "http://old:1/v1");
    text = ini();
    if (strstr(text, "old-model") || strstr(text, "http://old")) {
        printf("FAIL the old values stayed in the file:\n%s\n", text);
        failures++;
    }
    g_free(text);
    g_free(path);

    settings_save_profile("groq", "", "", "", NULL);
    settings_save_profile("custom", "", "", "", NULL);
    if (settings_load_profile("groq", &m, &u, &k) || settings_load_profile("custom", &m, &u, &k)) {
        printf("FAIL the test profiles were not deleted\n");
        failures++;
    }
    printf("%s\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
