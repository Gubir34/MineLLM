#ifndef MINELLM_SETTINGS_H
#define MINELLM_SETTINGS_H

#include <glib.h>

/* Settings live in $XDG_CONFIG_HOME/minellm/settings.ini; the login password
 * and the API keys are kept in the desktop keyring (Secret Service), never in
 * the file. Without a keyring they are kept for this session only. */
typedef struct {
    char *host;
    int port;
    char *username;
    char *password; /* keyring */

    char *provider;
    char *model;
    char *api_key; /* keyring, per provider */
    char *base_url;

    char *owners;         /* comma-separated players whose in-game chat the agent obeys */
    gboolean listen_chat; /* take orders from owners in game chat */
    int max_steps;        /* tool calls per request before the agent must stop */
} Settings;

Settings *settings_load(void);
/* Writes the file and the keyring entries; returns FALSE (with why) if the
 * keyring could not be used. */
gboolean settings_save(const Settings *s, char **why);
void settings_free(Settings *s);
/* The stored API key of a provider (to fill the field when it changes). */
char *settings_lookup_api_key(const char *provider);
/* Owner names, lowercased; free with g_strfreev. */
char **settings_owner_list(const Settings *s);

#endif
