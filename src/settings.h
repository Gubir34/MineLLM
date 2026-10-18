#ifndef MINELLM_SETTINGS_H
#define MINELLM_SETTINGS_H

#include <glib.h>

/* Settings live in $XDG_CONFIG_HOME/minellm/settings.ini. The login password
 * and, for every model provider, a profile (model, API address, API key) are
 * kept encrypted in the desktop keyring (Secret Service: KWallet, GNOME
 * Keyring...), never in the file, so switching providers brings each one's
 * model and key back. Without a keyring they last for this session only. */
typedef struct {
    char *host;
    int port;
    char *username;
    char *password; /* keyring */

    char *provider; /* the file keeps only which provider is in use */
    char *model;    /* the profile of that provider (keyring) */
    char *api_key;
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
/* A provider's saved profile; FALSE (and empty strings) if there is none. */
gboolean settings_load_profile(const char *provider, char **model, char **base_url, char **api_key);
/* Saves a provider's profile to the keyring (all empty: deletes it). */
gboolean settings_save_profile(const char *provider, const char *model, const char *base_url, const char *api_key,
                               char **why);
/* Switches s to another provider and its saved profile, and remembers the
 * choice in the file. */
void settings_use_provider(Settings *s, const char *provider);
/* Owner names, lowercased; free with g_strfreev. */
char **settings_owner_list(const Settings *s);

#endif
