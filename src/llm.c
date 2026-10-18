#include "llm.h"

#include <string.h>

const LlmProvider llm_providers[] = {
    {"ollama", "Ollama (local)", "http://127.0.0.1:11434", FALSE, NULL, "qwen3:8b"},
    {"openai", "OpenAI (ChatGPT)", "https://api.openai.com/v1", TRUE, "OPENAI_API_KEY", "model name"},
    {"anthropic", "Anthropic (Claude)", "https://api.anthropic.com/v1", TRUE, "ANTHROPIC_API_KEY", "claude-opus-5-5"},
    {"google", "Google AI (Gemini)", "https://generativelanguage.googleapis.com/v1beta/openai", TRUE, "GEMINI_API_KEY",
     "model name"},
    {"openrouter", "OpenRouter", "https://openrouter.ai/api/v1", TRUE, "OPENROUTER_API_KEY", "vendor/model"},
    {"groq", "Groq", "https://api.groq.com/openai/v1", TRUE, "GROQ_API_KEY", "model name"},
    {"custom", "OpenAI-compatible server", "", FALSE, NULL, "model name"},
};
const int llm_n_providers = G_N_ELEMENTS(llm_providers);

const LlmProvider *llm_provider(const char *id)
{
    for (int k = 0; k < llm_n_providers; k++)
        if (!g_strcmp0(llm_providers[k].id, id))
            return &llm_providers[k];
    return NULL;
}

static const char *base_url(const LlmConfig *c)
{
    if (c->base_url && *c->base_url)
        return c->base_url;
    const LlmProvider *p = llm_provider(c->provider);
    return p ? p->base_url : "";
}

static const char *api_key(const LlmConfig *c)
{
    if (c->api_key && *c->api_key)
        return c->api_key;
    const LlmProvider *p = llm_provider(c->provider);
    if (p && p->key_env) {
        const char *v = g_getenv(p->key_env);
        if (!v && !g_strcmp0(p->id, "google"))
            v = g_getenv("GOOGLE_API_KEY");
        return v;
    }
    return NULL;
}

char *llm_config_problem(const LlmConfig *c)
{
    const LlmProvider *p = llm_provider(c->provider);
    if (!p)
        return g_strdup_printf("unknown model provider \"%s\"", c->provider ? c->provider : "");
    if (!c->model || !*c->model)
        return g_strdup_printf("choose a model for %s in Preferences", p->label);
    if (!*base_url(c))
        return g_strdup("set the server address (base URL ending in /v1) in Preferences");
    if (p->needs_key && !api_key(c))
        return g_strdup_printf("%s needs an API key (Preferences, or %s)", p->label, p->key_env);
    return NULL;
}

/* ---- request bodies ---- */

static JsonNode *obj_node(JsonObject *o)
{
    return json_node_init_object(json_node_alloc(), o);
}

static JsonObject *openai_body(const LlmConfig *c, JsonArray *messages, JsonArray *tools)
{
    JsonObject *b = json_object_new();
    json_object_set_string_member(b, "model", c->model);
    json_object_set_array_member(b, "messages", json_array_ref(messages));
    if (tools && json_array_get_length(tools))
        json_object_set_array_member(b, "tools", json_array_ref(tools));
    int max = c->max_tokens > 0 ? c->max_tokens : 4096;
    /* OpenAI's reasoning models only accept max_completion_tokens */
    json_object_set_int_member(b, !g_strcmp0(c->provider, "openai") ? "max_completion_tokens" : "max_tokens", max);
    return b;
}

/* tool_call arguments arrive as a JSON string; Anthropic wants an object */
static JsonObject *args_object(const char *s)
{
    JsonNode *n = s && *s ? json_from_string(s, NULL) : NULL;
    JsonObject *o = n && JSON_NODE_HOLDS_OBJECT(n) ? json_object_ref(json_node_get_object(n)) : json_object_new();
    if (n)
        json_node_unref(n);
    return o;
}

/* Appends blocks to the last message if it has the same role, else a new message. */
static void push_blocks(JsonArray *out, const char *role, JsonArray *blocks)
{
    guint n = json_array_get_length(out);
    if (n) {
        JsonObject *last = json_array_get_object_element(out, n - 1);
        if (!g_strcmp0(json_object_get_string_member(last, "role"), role)) {
            JsonArray *have = json_object_get_array_member(last, "content");
            for (guint k = 0; k < json_array_get_length(blocks); k++)
                json_array_add_element(have, json_node_copy(json_array_get_element(blocks, k)));
            json_array_unref(blocks);
            return;
        }
    }
    JsonObject *m = json_object_new();
    json_object_set_string_member(m, "role", role);
    json_object_set_array_member(m, "content", blocks);
    json_array_add_object_element(out, m);
}

static JsonObject *text_block(const char *text)
{
    JsonObject *t = json_object_new();
    json_object_set_string_member(t, "type", "text");
    json_object_set_string_member(t, "text", text);
    return t;
}

static JsonObject *anthropic_body(const LlmConfig *c, JsonArray *messages, JsonArray *tools)
{
    JsonObject *b = json_object_new();
    json_object_set_string_member(b, "model", c->model);
    json_object_set_int_member(b, "max_tokens", c->max_tokens > 0 ? c->max_tokens : 4096);
    JsonArray *out = json_array_new();
    for (guint k = 0; k < json_array_get_length(messages); k++) {
        JsonObject *m = json_array_get_object_element(messages, k);
        const char *role = json_object_get_string_member_with_default(m, "role", "user");
        const char *content = json_object_has_member(m, "content") && !json_object_get_null_member(m, "content")
                                  ? json_object_get_string_member(m, "content")
                                  : NULL;
        if (!strcmp(role, "system")) {
            if (content)
                json_object_set_string_member(b, "system", content);
            continue;
        }
        JsonArray *blocks = json_array_new();
        if (!strcmp(role, "tool")) {
            JsonObject *r = json_object_new();
            json_object_set_string_member(r, "type", "tool_result");
            json_object_set_string_member(r, "tool_use_id", json_object_get_string_member_with_default(m, "tool_call_id", ""));
            json_object_set_string_member(r, "content", content ? content : "");
            json_array_add_object_element(blocks, r);
            push_blocks(out, "user", blocks);
            continue;
        }
        if (content && *content)
            json_array_add_object_element(blocks, text_block(content));
        if (!strcmp(role, "assistant") && json_object_has_member(m, "tool_calls")) {
            JsonArray *calls = json_object_get_array_member(m, "tool_calls");
            for (guint i = 0; i < json_array_get_length(calls); i++) {
                JsonObject *call = json_array_get_object_element(calls, i);
                JsonObject *fn = json_object_get_object_member(call, "function");
                JsonObject *u = json_object_new();
                json_object_set_string_member(u, "type", "tool_use");
                json_object_set_string_member(u, "id", json_object_get_string_member(call, "id"));
                json_object_set_string_member(u, "name", json_object_get_string_member(fn, "name"));
                json_object_set_object_member(u, "input", args_object(json_object_get_string_member_with_default(fn, "arguments", "")));
                json_array_add_object_element(blocks, u);
            }
        }
        if (json_array_get_length(blocks) == 0)
            json_array_add_object_element(blocks, text_block("(empty)"));
        push_blocks(out, !strcmp(role, "assistant") ? "assistant" : "user", blocks);
    }
    json_object_set_array_member(b, "messages", out);
    if (tools && json_array_get_length(tools)) {
        JsonArray *at = json_array_new();
        for (guint k = 0; k < json_array_get_length(tools); k++) {
            JsonObject *fn = json_object_get_object_member(json_array_get_object_element(tools, k), "function");
            JsonObject *t = json_object_new();
            json_object_set_string_member(t, "name", json_object_get_string_member(fn, "name"));
            json_object_set_string_member(t, "description", json_object_get_string_member_with_default(fn, "description", ""));
            json_object_set_member(t, "input_schema", json_node_copy(json_object_get_member(fn, "parameters")));
            json_array_add_object_element(at, t);
        }
        json_object_set_array_member(b, "tools", at);
    }
    return b;
}

/* Ollama's own /api/chat: unlike its /v1 endpoint it takes the context size,
 * and its default (4096 tokens) is too small for the tool definitions. */
#define OLLAMA_CTX 16384

static JsonObject *ollama_body(const LlmConfig *c, JsonArray *messages, JsonArray *tools)
{
    JsonObject *b = json_object_new();
    json_object_set_string_member(b, "model", c->model);
    json_object_set_boolean_member(b, "stream", FALSE);
    JsonArray *out = json_array_new();
    for (guint k = 0; k < json_array_get_length(messages); k++) {
        JsonObject *m = json_array_get_object_element(messages, k);
        JsonObject *o = json_object_new();
        const char *role = json_object_get_string_member_with_default(m, "role", "user");
        json_object_set_string_member(o, "role", role);
        const char *content = json_object_has_member(m, "content") && !json_object_get_null_member(m, "content")
                                  ? json_object_get_string_member(m, "content")
                                  : "";
        json_object_set_string_member(o, "content", content ? content : "");
        if (json_object_has_member(m, "tool_calls")) {
            JsonArray *calls = json_object_get_array_member(m, "tool_calls"), *oc = json_array_new();
            for (guint i = 0; i < json_array_get_length(calls); i++) {
                JsonObject *fn = json_object_get_object_member(json_array_get_object_element(calls, i), "function");
                JsonObject *f = json_object_new(), *call = json_object_new();
                json_object_set_string_member(f, "name", json_object_get_string_member(fn, "name"));
                json_object_set_object_member(f, "arguments", args_object(json_object_get_string_member_with_default(fn, "arguments", "")));
                json_object_set_object_member(call, "function", f);
                json_array_add_object_element(oc, call);
            }
            json_object_set_array_member(o, "tool_calls", oc);
        }
        json_array_add_object_element(out, o);
    }
    json_object_set_array_member(b, "messages", out);
    if (tools && json_array_get_length(tools))
        json_object_set_array_member(b, "tools", json_array_ref(tools));
    JsonObject *opt = json_object_new();
    json_object_set_int_member(opt, "num_ctx", OLLAMA_CTX);
    json_object_set_int_member(opt, "num_predict", c->max_tokens > 0 ? c->max_tokens : 4096);
    json_object_set_object_member(b, "options", opt);
    return b;
}

/* ---- replies ---- */

/* Drops the <think>...</think> some local models put in the content. */
static char *strip_think(const char *s)
{
    GString *g = g_string_new(s);
    char *a;
    while ((a = strstr(g->str, "<think>"))) {
        char *e = strstr(a, "</think>");
        gsize from = a - g->str, to = e ? (gsize)(e - g->str) + 8 : g->len;
        g_string_erase(g, from, to - from);
    }
    char *r = g_strstrip(g_string_free(g, FALSE));
    return r;
}

static JsonObject *openai_reply(JsonObject *body, char **err)
{
    JsonArray *choices = json_object_has_member(body, "choices") ? json_object_get_array_member(body, "choices") : NULL;
    if (!choices || !json_array_get_length(choices)) {
        *err = g_strdup("the model returned no choices");
        return NULL;
    }
    JsonObject *msg = json_object_get_object_member(json_array_get_object_element(choices, 0), "message");
    if (!msg) {
        *err = g_strdup("the model returned no message");
        return NULL;
    }
    JsonObject *a = json_object_new();
    json_object_set_string_member(a, "role", "assistant");
    const char *content = json_object_has_member(msg, "content") && !json_object_get_null_member(msg, "content")
                              ? json_object_get_string_member(msg, "content")
                              : NULL;
    char *clean = content ? strip_think(content) : NULL;
    if (clean && *clean)
        json_object_set_string_member(a, "content", clean);
    else
        json_object_set_null_member(a, "content");
    g_free(clean);
    /* Gemini puts its thought signatures in extra_content (on the message and on
     * each tool call) and refuses the next request unless they come back as they were. */
    if (json_object_has_member(msg, "extra_content"))
        json_object_set_member(a, "extra_content", json_node_copy(json_object_get_member(msg, "extra_content")));
    if (json_object_has_member(msg, "tool_calls") && !json_object_get_null_member(msg, "tool_calls")) {
        JsonArray *calls = json_object_get_array_member(msg, "tool_calls");
        JsonArray *out = json_array_new();
        for (guint k = 0; k < json_array_get_length(calls); k++) {
            JsonObject *call = json_array_get_object_element(calls, k);
            JsonObject *fn = json_object_get_object_member(call, "function");
            if (!fn)
                continue;
            JsonObject *c = json_object_new();
            const char *id = json_object_get_string_member_with_default(call, "id", "");
            char *gen = *id ? NULL : g_strdup_printf("call_%u", g_random_int());
            json_object_set_string_member(c, "id", gen ? gen : id);
            g_free(gen);
            json_object_set_string_member(c, "type", "function");
            JsonObject *f = json_object_new();
            json_object_set_string_member(f, "name", json_object_get_string_member_with_default(fn, "name", ""));
            /* arguments is a JSON string, but some servers (Ollama) may send an object */
            JsonNode *an = json_object_get_member(fn, "arguments");
            if (an && JSON_NODE_HOLDS_OBJECT(an)) {
                char *s = json_to_string(an, FALSE);
                json_object_set_string_member(f, "arguments", s);
                g_free(s);
            } else {
                json_object_set_string_member(f, "arguments", an && JSON_NODE_HOLDS_VALUE(an) ? json_node_get_string(an) : "{}");
            }
            json_object_set_object_member(c, "function", f);
            if (json_object_has_member(call, "extra_content"))
                json_object_set_member(c, "extra_content", json_node_copy(json_object_get_member(call, "extra_content")));
            json_array_add_object_element(out, c);
        }
        if (json_array_get_length(out))
            json_object_set_array_member(a, "tool_calls", out);
        else
            json_array_unref(out);
    }
    return a;
}

/* {"message":{...}} has the shape of an OpenAI choice, with object arguments
 * and no call ids; openai_reply handles both. */
static JsonObject *openai_reply(JsonObject *body, char **err);
static JsonObject *ollama_reply(JsonObject *body, char **err)
{
    if (!json_object_has_member(body, "message")) {
        *err = g_strdup("the model returned no message");
        return NULL;
    }
    JsonObject *wrap = json_object_new();
    JsonArray *choices = json_array_new();
    JsonObject *choice = json_object_new();
    json_object_set_member(choice, "message", json_node_copy(json_object_get_member(body, "message")));
    json_array_add_object_element(choices, choice);
    json_object_set_array_member(wrap, "choices", choices);
    JsonObject *r = openai_reply(wrap, err);
    json_object_unref(wrap);
    return r;
}

static JsonObject *anthropic_reply(JsonObject *body, char **err)
{
    JsonArray *content = json_object_has_member(body, "content") ? json_object_get_array_member(body, "content") : NULL;
    if (!content) {
        *err = g_strdup("the model returned no content");
        return NULL;
    }
    JsonObject *a = json_object_new();
    json_object_set_string_member(a, "role", "assistant");
    GString *text = g_string_new(NULL);
    JsonArray *calls = json_array_new();
    for (guint k = 0; k < json_array_get_length(content); k++) {
        JsonObject *blk = json_array_get_object_element(content, k);
        const char *type = json_object_get_string_member_with_default(blk, "type", "");
        if (!strcmp(type, "text")) {
            if (text->len)
                g_string_append_c(text, '\n');
            g_string_append(text, json_object_get_string_member_with_default(blk, "text", ""));
        } else if (!strcmp(type, "tool_use")) {
            JsonObject *c = json_object_new();
            json_object_set_string_member(c, "id", json_object_get_string_member(blk, "id"));
            json_object_set_string_member(c, "type", "function");
            JsonObject *f = json_object_new();
            json_object_set_string_member(f, "name", json_object_get_string_member(blk, "name"));
            char *args = json_to_string(json_object_get_member(blk, "input"), FALSE);
            json_object_set_string_member(f, "arguments", args);
            g_free(args);
            json_object_set_object_member(c, "function", f);
            json_array_add_object_element(calls, c);
        }
    }
    if (text->len)
        json_object_set_string_member(a, "content", text->str);
    else
        json_object_set_null_member(a, "content");
    g_string_free(text, TRUE);
    if (json_array_get_length(calls))
        json_object_set_array_member(a, "tool_calls", calls);
    else
        json_array_unref(calls);
    return a;
}

typedef struct {
    LlmReplyFn cb;
    gpointer ud;
    gboolean anthropic, ollama;
    char *provider;
    SoupMessage *msg;
} Call;

static char *error_text(const char *provider, guint status, GBytes *bytes)
{
    gsize len = 0;
    const char *data = bytes ? g_bytes_get_data(bytes, &len) : NULL;
    char *detail = NULL;
    if (data && len) {
        JsonNode *n = json_from_string(data, NULL);
        if (n && JSON_NODE_HOLDS_OBJECT(n)) {
            JsonObject *o = json_node_get_object(n);
            JsonNode *e = json_object_get_member(o, "error");
            if (e && JSON_NODE_HOLDS_OBJECT(e))
                detail = g_strdup(json_object_get_string_member_with_default(json_node_get_object(e), "message", ""));
            else if (e && JSON_NODE_HOLDS_VALUE(e))
                detail = g_strdup(json_node_get_string(e));
        }
        if (n)
            json_node_unref(n);
        if (!detail || !*detail) {
            g_free(detail);
            detail = g_strndup(data, MIN(len, 300));
        }
    }
    char *r = g_strdup_printf("%s API error %u: %s", provider, status, detail ? detail : soup_status_get_phrase(status));
    g_free(detail);
    return r;
}

static void on_reply(GObject *src, GAsyncResult *res, gpointer ud)
{
    Call *call = ud;
    GError *err = NULL;
    GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(src), res, &err);
    if (!bytes) {
        char *m = g_strdup_printf("%s: %s", call->provider, err->message);
        call->cb(NULL, g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED) ? "cancelled" : m, call->ud);
        g_free(m);
        g_error_free(err);
        goto out;
    }
    guint status = soup_message_get_status(call->msg);
    if (status < 200 || status >= 300) {
        char *m = error_text(call->provider, status, bytes);
        call->cb(NULL, m, call->ud);
        g_free(m);
        g_bytes_unref(bytes);
        goto out;
    }
    gsize len;
    const char *data = g_bytes_get_data(bytes, &len);
    char *copy = g_strndup(data, len);
    if (g_getenv("MINELLM_DEBUG"))
        g_printerr("[llm] %s reply: %.3000s\n", call->provider, copy);
    JsonNode *n = json_from_string(copy, &err);
    g_free(copy);
    g_bytes_unref(bytes);
    if (!n || !JSON_NODE_HOLDS_OBJECT(n)) {
        char *m = g_strdup_printf("%s: unreadable reply%s%s", call->provider, err ? ": " : "", err ? err->message : "");
        call->cb(NULL, m, call->ud);
        g_free(m);
        g_clear_error(&err);
        if (n)
            json_node_unref(n);
        goto out;
    }
    char *perr = NULL;
    JsonObject *body = json_node_get_object(n);
    JsonObject *a = call->anthropic ? anthropic_reply(body, &perr)
                    : call->ollama  ? ollama_reply(body, &perr)
                                    : openai_reply(body, &perr);
    call->cb(a, perr, call->ud);
    if (a)
        json_object_unref(a);
    g_free(perr);
    json_node_unref(n);
out:
    g_object_unref(call->msg);
    g_free(call->provider);
    g_free(call);
}

void llm_chat_async(SoupSession *session, const LlmConfig *c, JsonArray *messages, JsonArray *tools,
                    GCancellable *cancel, LlmReplyFn cb, gpointer user_data)
{
    char *problem = llm_config_problem(c);
    if (problem) {
        cb(NULL, problem, user_data);
        g_free(problem);
        return;
    }
    gboolean anthropic = !g_strcmp0(c->provider, "anthropic"), ollama = !g_strcmp0(c->provider, "ollama");
    char *base = g_strdup(base_url(c));
    while (*base && base[strlen(base) - 1] == '/')
        base[strlen(base) - 1] = 0;
    if (ollama && g_str_has_suffix(base, "/v1"))
        base[strlen(base) - 3] = 0; /* an OpenAI-style address: use the native API next to it */
    char *url = g_strconcat(base, anthropic ? "/messages" : ollama ? "/api/chat" : "/chat/completions", NULL);
    g_free(base);
    SoupMessage *msg = soup_message_new("POST", url);
    if (!msg) {
        char *m = g_strdup_printf("invalid API address %s", url);
        cb(NULL, m, user_data);
        g_free(m);
        g_free(url);
        return;
    }
    g_free(url);
    SoupMessageHeaders *h = soup_message_get_request_headers(msg);
    const char *key = api_key(c);
    if (anthropic) {
        soup_message_headers_append(h, "x-api-key", key ? key : "");
        soup_message_headers_append(h, "anthropic-version", "2023-06-01");
    } else if (key && *key) {
        char *auth = g_strconcat("Bearer ", key, NULL);
        soup_message_headers_append(h, "Authorization", auth);
        g_free(auth);
    }
    if (!g_strcmp0(c->provider, "openrouter"))
        soup_message_headers_append(h, "X-Title", "MineLLM");
    JsonObject *body = anthropic ? anthropic_body(c, messages, tools)
                       : ollama  ? ollama_body(c, messages, tools)
                                 : openai_body(c, messages, tools);
    JsonNode *bn = obj_node(body);
    gsize len;
    char *s = json_to_string(bn, FALSE);
    len = strlen(s);
    json_node_unref(bn);
    json_object_unref(body);
    GBytes *bytes = g_bytes_new_take(s, len);
    soup_message_set_request_body_from_bytes(msg, "application/json", bytes);
    g_bytes_unref(bytes);

    Call *call = g_new0(Call, 1);
    call->cb = cb;
    call->ud = user_data;
    call->anthropic = anthropic;
    call->ollama = ollama;
    call->provider = g_strdup(llm_provider(c->provider)->label);
    call->msg = msg;
    soup_session_send_and_read_async(session, msg, G_PRIORITY_DEFAULT, cancel, on_reply, call);
}
