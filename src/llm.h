#ifndef MINELLM_LLM_H
#define MINELLM_LLM_H

#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

/* Chat completions with tool calling. Conversations are kept in the OpenAI
 * chat format (system / user / assistant with tool_calls / tool) and
 * converted for Anthropic's Messages API when that provider is used. Every
 * other provider is reached through an OpenAI-compatible endpoint. */

typedef struct {
    const char *id;           /* settings value */
    const char *label;        /* shown in the UI */
    const char *base_url;     /* default API base, "" = must be given */
    gboolean needs_key;
    const char *key_env;      /* environment variable read when no key is stored */
    const char *model_hint;   /* placeholder for the model field */
    const char *default_model; /* used when no model is set, or NULL */
} LlmProvider;

extern const LlmProvider llm_providers[];
extern const int llm_n_providers;
const LlmProvider *llm_provider(const char *id); /* NULL if unknown */

typedef struct {
    char *provider, *model, *api_key, *base_url; /* base_url NULL/"" = the provider's default */
    int max_tokens;
} LlmConfig;

/* The model a request uses: the configured one, else the provider's default. */
const char *llm_model(const LlmConfig *c);
/* Checks the configuration; returns an error message (to free) or NULL. */
char *llm_config_problem(const LlmConfig *c);

/* The reply as an OpenAI-format assistant message ({"role":"assistant",
 * "content":..., "tool_calls":[{"id","type":"function","function":{"name",
 * "arguments": "<json>"}}]}), or NULL and an error. */
typedef void (*LlmReplyFn)(JsonObject *assistant, const char *error, gpointer user_data);

/* messages: the conversation (the first may be the system message);
 * tools: OpenAI-format tool definitions ({"type":"function","function":{...}}). */
void llm_chat_async(SoupSession *session, const LlmConfig *c, JsonArray *messages, JsonArray *tools,
                    GCancellable *cancel, LlmReplyFn cb, gpointer user_data);

#endif
