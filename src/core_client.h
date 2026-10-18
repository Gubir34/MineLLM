#ifndef MINELLM_CORE_CLIENT_H
#define MINELLM_CORE_CLIENT_H

#include <json-glib/json-glib.h>

/* Talks to the minemcp core daemon (the MineMCP C core) over its Unix-socket
 * JSON-lines control protocol, starting it as a child process when needed.
 * Requests are {"id", "cmd", ...args}; a reply carrying a "task" number
 * finishes later with a task_done event. Everything runs on the GLib main
 * loop. */

typedef struct CoreClient CoreClient;

/* Every event line from the core ({"event": ...}). */
typedef void (*CoreEventFn)(JsonObject *event, gpointer user_data);
/* Diagnostic lines (the core's stderr and our own). */
typedef void (*CoreLogFn)(const char *line, gpointer user_data);
/* reply: the reply object (or the task_done event for tasks); NULL on error. */
typedef void (*CoreReplyFn)(JsonObject *reply, const char *error, gpointer user_data);

CoreClient *core_client_new(const char *binary, const char *socket_path, CoreEventFn on_event, CoreLogFn on_log,
                            gpointer user_data);
void core_client_free(CoreClient *c); /* shuts down a core it started */

/* The login password handed to the core through its environment (never on
 * the command line). Takes effect the next time the core is started. */
void core_client_set_password(CoreClient *c, const char *password);
/* Whether a core started with this password is running. */
gboolean core_client_running_with(CoreClient *c, const char *password);
/* Stops the core (if this client started it) so the next request restarts it. */
void core_client_restart(CoreClient *c);

/* Sends one request; args may be NULL and is not taken. The reply comes
 * through cb. timeout_s bounds the wait for the reply. */
void core_client_request(CoreClient *c, const char *cmd, JsonObject *args, int timeout_s, CoreReplyFn cb, gpointer ud);
/* Like core_client_request, but when the reply starts a task, waits for its
 * task_done event (up to timeout_s, then sends stop). */
void core_client_run(CoreClient *c, const char *cmd, JsonObject *args, int timeout_s, CoreReplyFn cb, gpointer ud);

const char *core_client_socket_path(CoreClient *c);
/* $XDG_RUNTIME_DIR/minellm-<uid>.sock */
char *core_default_socket_path(void);
/* minemcp next to our own executable, else "minemcp" from PATH. */
char *core_default_binary(void);

#endif
