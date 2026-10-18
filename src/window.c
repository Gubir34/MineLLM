#include "window.h"

#include "agent.h"
#include "core_client.h"
#include "llm.h"
#include "settings.h"

#include <string.h>

#define LOG_MAX_LINES 3000
#define CHAT_MAX_LINES 1000

typedef struct {
    AdwApplication *app;
    GtkWindow *win;
    Settings *settings;
    CoreClient *core;
    SoupSession *http;
    Agent *agent;

    AdwToastOverlay *toasts;
    AdwWindowTitle *title;
    AdwBanner *banner;
    GtkWidget *connect_btn, *spinner, *stop_btn, *send_btn, *entry;
    GtkWidget *convo_box, *convo_scroll;
    GtkWidget *row_health, *row_food, *row_pos, *row_dim, *row_task, *row_reflex;
    GtkTextView *chat_view, *log_view;
    GtkWidget *chat_entry;

    gboolean connected, spawned, connecting;
    guint status_timer;
    char *player_name; /* the bot's name in game, once spawned */

    /* preferences dialog fields, while it is open */
    GtkWidget *p_host, *p_port, *p_user, *p_pass, *p_provider, *p_model, *p_key, *p_url, *p_owners, *p_listen, *p_steps;
    char *p_last_provider;
} App;

static App *A; /* one window per application instance */

/* ---- small helpers ---- */

static void toast(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void toast(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *s = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    AdwToast *t = adw_toast_new(s);
    adw_toast_set_timeout(t, 5);
    adw_toast_overlay_add_toast(A->toasts, t);
    g_free(s);
}

static void text_append(GtkTextView *view, const char *line, int max_lines)
{
    GtkTextBuffer *buf = gtk_text_view_get_buffer(view);
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buf, &end);
    if (gtk_text_buffer_get_char_count(buf) > 0)
        gtk_text_buffer_insert(buf, &end, "\n", 1);
    gtk_text_buffer_insert(buf, &end, line, -1);
    int lines = gtk_text_buffer_get_line_count(buf);
    if (lines > max_lines) {
        GtkTextIter a, b;
        gtk_text_buffer_get_start_iter(buf, &a);
        gtk_text_buffer_get_iter_at_line(buf, &b, lines - max_lines * 3 / 4);
        gtk_text_buffer_delete(buf, &a, &b);
    }
    GtkTextMark *mark = gtk_text_buffer_get_mark(buf, "end");
    if (!mark) {
        gtk_text_buffer_get_end_iter(buf, &end);
        mark = gtk_text_buffer_create_mark(buf, "end", &end, FALSE);
    }
    gtk_text_view_scroll_mark_onscreen(view, mark);
}

static void log_line(const char *line)
{
    GDateTime *now = g_date_time_new_now_local();
    char *t = g_date_time_format(now, "%H:%M:%S");
    g_date_time_unref(now);
    char *s = g_strdup_printf("%s %s", t, line);
    text_append(A->log_view, s, LOG_MAX_LINES);
    g_free(s);
    g_free(t);
}

static GtkWidget *view_text(gboolean mono)
{
    GtkWidget *v = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(v), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(v), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(v), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(v), mono);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(v), 8);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(v), 8);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(v), 6);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(v), 6);
    return v;
}

static GtkWidget *scrolled(GtkWidget *child)
{
    GtkWidget *s = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(s), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(s), child);
    gtk_widget_set_vexpand(s, TRUE);
    return s;
}

/* ---- the conversation pane ---- */

static gboolean scroll_bottom(gpointer p)
{
    (void)p;
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(A->convo_scroll));
    gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj));
    return G_SOURCE_REMOVE;
}

static GtkWidget *wrap_label(const char *text, const char *css)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(l), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_selectable(GTK_LABEL(l), TRUE);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    if (css)
        gtk_widget_add_css_class(l, css);
    return l;
}

static char *shorten(const char *s, int max)
{
    char *one = g_strdup(s);
    for (char *p = one; *p; p++)
        if (*p == '\n' || *p == '\r' || *p == '\t')
            *p = ' ';
    if (g_utf8_strlen(one, -1) <= max)
        return one;
    char *end = g_utf8_offset_to_pointer(one, max);
    char *r = g_strdup_printf("%.*s…", (int)(end - one), one);
    g_free(one);
    return r;
}

static void on_agent_entry(AgentEntry kind, const char *title, const char *text, gboolean ok, gpointer ud)
{
    (void)ud;
    GtkWidget *w = NULL;
    switch (kind) {
    case AGENT_USER: {
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_add_css_class(box, "user-bubble");
        gtk_widget_set_halign(box, GTK_ALIGN_END);
        gtk_widget_set_margin_start(box, 60);
        GtkWidget *who = gtk_label_new(title);
        gtk_widget_add_css_class(who, "caption-heading");
        gtk_label_set_xalign(GTK_LABEL(who), 0);
        gtk_box_append(GTK_BOX(box), who);
        gtk_box_append(GTK_BOX(box), wrap_label(text, NULL));
        w = box;
        break;
    }
    case AGENT_REPLY:
        w = wrap_label(text, "reply-bubble");
        gtk_widget_set_halign(w, GTK_ALIGN_START);
        gtk_widget_set_margin_end(w, 60);
        break;
    case AGENT_TOOL_CALL: {
        char *args = shorten(text, 160);
        char *s = g_strdup_printf("▸ %s %s", title, !strcmp(args, "{}") ? "" : args);
        w = wrap_label(s, "tool-call");
        g_free(s);
        g_free(args);
        break;
    }
    case AGENT_TOOL_RESULT: {
        char *head = shorten(text, 140);
        char *s = g_strdup_printf("%s %s → %s", ok ? "✓" : "✗", title, head);
        GtkWidget *exp = gtk_expander_new(s);
        gtk_widget_add_css_class(exp, ok ? "tool-ok" : "tool-fail");
        GtkWidget *lbl = gtk_expander_get_label_widget(GTK_EXPANDER(exp));
        if (GTK_IS_LABEL(lbl)) {
            gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
            gtk_label_set_xalign(GTK_LABEL(lbl), 0);
        }
        gtk_expander_set_child(GTK_EXPANDER(exp), wrap_label(text, "tool-detail"));
        w = exp;
        g_free(s);
        g_free(head);
        break;
    }
    case AGENT_ERROR:
        w = wrap_label(text, "error");
        break;
    case AGENT_INFO:
        w = wrap_label(text, "dim-label");
        gtk_label_set_selectable(GTK_LABEL(w), FALSE);
        break;
    }
    gtk_box_append(GTK_BOX(A->convo_box), w);
    g_timeout_add(60, scroll_bottom, NULL);
}

static void on_agent_busy(gboolean busy, gpointer ud)
{
    (void)ud;
    gtk_widget_set_visible(A->spinner, busy);
    gtk_widget_set_sensitive(A->stop_btn, busy);
}

static void submit_prompt(void)
{
    const char *t = gtk_editable_get_text(GTK_EDITABLE(A->entry));
    char *text = g_strstrip(g_strdup(t));
    if (*text) {
        agent_submit(A->agent, text, NULL);
        gtk_editable_set_text(GTK_EDITABLE(A->entry), "");
    }
    g_free(text);
}

/* ---- configuration ---- */

static void fill_llm(LlmConfig *c)
{
    c->provider = A->settings->provider;
    c->model = A->settings->model;
    c->api_key = A->settings->api_key;
    c->base_url = A->settings->base_url;
    c->max_tokens = 4096;
}

static void apply_settings(void)
{
    LlmConfig c;
    fill_llm(&c);
    char **owners = settings_owner_list(A->settings);
    agent_configure(A->agent, &c, A->settings->max_steps, A->player_name ? A->player_name : A->settings->username, owners);
    g_strfreev(owners);
    char *problem = llm_config_problem(&c);
    if (problem) {
        char *t = g_strdup_printf("Model: %s", problem);
        adw_banner_set_title(A->banner, t);
        g_free(t);
    }
    adw_banner_set_revealed(A->banner, problem != NULL);
    g_free(problem);
}

/* ---- connection and status ---- */

static void update_title(void)
{
    char *sub;
    if (A->spawned)
        sub = g_strdup_printf("%s on %s:%d", A->player_name ? A->player_name : A->settings->username, A->settings->host,
                              A->settings->port);
    else if (A->connecting)
        sub = g_strdup_printf("connecting to %s:%d…", A->settings->host, A->settings->port);
    else
        sub = g_strdup("not connected");
    adw_window_title_set_subtitle(A->title, sub);
    g_free(sub);
    gtk_button_set_label(GTK_BUTTON(A->connect_btn), A->connected || A->connecting ? "Disconnect" : "Connect");
    if (A->connected || A->connecting) {
        gtk_widget_remove_css_class(A->connect_btn, "suggested-action");
        gtk_widget_add_css_class(A->connect_btn, "destructive-action");
    } else {
        gtk_widget_remove_css_class(A->connect_btn, "destructive-action");
        gtk_widget_add_css_class(A->connect_btn, "suggested-action");
    }
}

static void set_row(GtkWidget *row, const char *value)
{
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), value && *value ? value : "–");
}

static void on_status(JsonObject *reply, const char *error, gpointer ud)
{
    (void)ud, (void)error;
    if (!reply || !A)
        return;
    JsonObject *s = json_object_get_object_member(reply, "status");
    if (!s)
        return;
    gboolean connected = json_object_get_boolean_member_with_default(s, "connected", FALSE);
    gboolean spawned = json_object_get_boolean_member_with_default(s, "spawned", FALSE);
    const char *uname = json_object_get_string_member_with_default(s, "username", NULL);
    if (spawned && uname && g_strcmp0(uname, A->player_name)) {
        g_free(A->player_name);
        A->player_name = g_strdup(uname);
        apply_settings(); /* the agent knows its name */
        update_title();
    }
    if (connected != A->connected || spawned != A->spawned) {
        A->connected = connected;
        A->spawned = spawned;
        if (connected)
            A->connecting = FALSE;
        update_title();
    }
    char buf[160];
    if (json_object_has_member(s, "health")) {
        snprintf(buf, sizeof buf, "%.1f / 20", json_object_get_double_member(s, "health"));
        set_row(A->row_health, buf);
        snprintf(buf, sizeof buf, "%d / 20 (saturation %.1f)", (int)json_object_get_int_member(s, "food"),
                 json_object_get_double_member_with_default(s, "saturation", 0));
        set_row(A->row_food, buf);
    } else {
        set_row(A->row_health, NULL);
        set_row(A->row_food, NULL);
    }
    JsonObject *p = json_object_has_member(s, "position") ? json_object_get_object_member(s, "position") : NULL;
    if (p) {
        snprintf(buf, sizeof buf, "%.1f  %.1f  %.1f", json_object_get_double_member(p, "x"), json_object_get_double_member(p, "y"),
                 json_object_get_double_member(p, "z"));
        set_row(A->row_pos, buf);
    } else {
        set_row(A->row_pos, NULL);
    }
    set_row(A->row_dim, json_object_get_string_member_with_default(s, "dimension", NULL));
    JsonNode *tn = json_object_get_member(s, "task");
    if (tn && JSON_NODE_HOLDS_OBJECT(tn)) {
        JsonObject *t = json_node_get_object(tn);
        snprintf(buf, sizeof buf, "%s (%d s)", json_object_get_string_member_with_default(t, "kind", "?"),
                 (int)json_object_get_int_member_with_default(t, "seconds", 0));
        set_row(A->row_task, buf);
    } else {
        set_row(A->row_task, connected ? "idle" : NULL);
    }
    JsonNode *rn = json_object_get_member(s, "reflexes");
    set_row(A->row_reflex, rn && JSON_NODE_HOLDS_OBJECT(rn)
                               ? json_object_get_string_member_with_default(json_node_get_object(rn), "state", NULL)
                               : NULL);
}

static gboolean status_tick(gpointer p)
{
    (void)p;
    if (!A->connected && !A->connecting) {
        A->status_timer = 0;
        return G_SOURCE_REMOVE;
    }
    core_client_request(A->core, "status", NULL, 5, on_status, NULL);
    return G_SOURCE_CONTINUE;
}

static void start_status_timer(void)
{
    if (!A->status_timer)
        A->status_timer = g_timeout_add_seconds(1, status_tick, NULL);
}

static void on_connect_reply(JsonObject *reply, const char *error, gpointer ud)
{
    (void)ud;
    if (!reply) {
        A->connecting = FALSE;
        update_title();
        toast("Cannot connect: %s", error);
        log_line(error);
    }
}

static void on_disconnect_reply(JsonObject *reply, const char *error, gpointer ud)
{
    (void)reply, (void)ud;
    if (error)
        toast("%s", error);
}

static void connect_clicked(GtkButton *b, gpointer ud)
{
    (void)b, (void)ud;
    if (A->connected || A->connecting) {
        A->connecting = FALSE;
        core_client_request(A->core, "disconnect", NULL, 10, on_disconnect_reply, NULL);
        return;
    }
    if (!core_client_running_with(A->core, A->settings->password)) {
        core_client_set_password(A->core, A->settings->password);
        core_client_restart(A->core); /* the password reaches the core only when it starts */
    }
    JsonObject *args = json_object_new();
    json_object_set_string_member(args, "host", A->settings->host);
    json_object_set_int_member(args, "port", A->settings->port);
    json_object_set_string_member(args, "username", A->settings->username);
    A->connecting = TRUE;
    update_title();
    core_client_request(A->core, "connect", args, 30, on_connect_reply, NULL);
    json_object_unref(args);
    start_status_timer();
}

/* ---- in-game chat ---- */

static gboolean is_owner(const char *name)
{
    if (!name)
        return FALSE;
    char **owners = settings_owner_list(A->settings);
    char *low = g_ascii_strdown(name, -1);
    gboolean yes = g_strv_contains((const char *const *)owners, low);
    g_free(low);
    g_strfreev(owners);
    return yes;
}

/* An order to the bot from an owner: a whisper, a message naming the bot, or
 * one starting with "!". Returns the order text (to free) or NULL. */
static char *owner_order(JsonObject *ev, char **who)
{
    const char *text = json_object_get_string_member_with_default(ev, "text", "");
    const char *sender = json_object_get_string_member_with_default(ev, "sender", NULL);
    const char *me = A->player_name ? A->player_name : A->settings->username;
    char *name = NULL, *body = NULL;
    gboolean whisper = FALSE;
    GMatchInfo *mi = NULL;
    static GRegex *re_whisper, *re_whisper2, *re_line;
    if (!re_whisper) {
        re_whisper = g_regex_new("^(\\S{1,16}) whispers to you: (.*)$", G_REGEX_CASELESS, 0, NULL);
        re_whisper2 = g_regex_new("^\\[(\\S{1,16}) -> (?:me|you)\\] (.*)$", G_REGEX_CASELESS, 0, NULL);
        re_line = g_regex_new("^(?:\\[[^\\]]*\\]\\s*)*<?([A-Za-z0-9_]{3,16})>?\\s*(?::|»|>)\\s*(.+)$", 0, 0, NULL);
    }
    if (g_regex_match(re_whisper, text, 0, &mi) || (g_match_info_free(mi), g_regex_match(re_whisper2, text, 0, &mi))) {
        name = g_match_info_fetch(mi, 1);
        body = g_match_info_fetch(mi, 2);
        whisper = TRUE;
    } else if (sender) {
        name = g_strdup(sender);
        const char *at = strstr(text, sender);
        body = g_strdup(at && at - text < 40 ? at + strlen(sender) : text);
        g_strstrip(body);
        while (*body && strchr(">]:» ", *body))
            memmove(body, body + 1, strlen(body));
    } else if (g_match_info_free(mi), g_regex_match(re_line, text, 0, &mi)) {
        name = g_match_info_fetch(mi, 1);
        body = g_match_info_fetch(mi, 2);
    }
    g_match_info_free(mi);
    char *order = NULL;
    if (name && body && g_ascii_strcasecmp(name, me) && is_owner(name)) {
        char *low = g_ascii_strdown(body, -1), *melow = g_ascii_strdown(me, -1);
        if (whisper)
            order = g_strdup(body);
        else if (body[0] == '!')
            order = g_strdup(body + 1);
        else if (strstr(low, melow))
            order = g_strdup(body);
        g_free(low);
        g_free(melow);
    }
    if (order) {
        g_strstrip(order);
        if (!*order) {
            g_free(order);
            order = NULL;
        }
    }
    if (order)
        *who = g_strdup(name);
    g_free(name);
    g_free(body);
    return order;
}

static void on_chat_reply(JsonObject *reply, const char *error, gpointer ud)
{
    (void)reply, (void)ud;
    if (error)
        toast("Not sent: %s", error);
}

static void chat_send(GtkEntry *e, gpointer ud)
{
    (void)ud;
    const char *t = gtk_editable_get_text(GTK_EDITABLE(e));
    if (!*t)
        return;
    JsonObject *args = json_object_new();
    json_object_set_string_member(args, "message", t);
    core_client_request(A->core, "chat", args, 10, on_chat_reply, NULL);
    json_object_unref(args);
    gtk_editable_set_text(GTK_EDITABLE(e), "");
}

/* ---- core events ---- */

static void on_core_event(JsonObject *ev, gpointer ud)
{
    (void)ud;
    const char *name = json_object_get_string_member_with_default(ev, "event", "");
    if (!strcmp(name, "chat")) {
        const char *kind = json_object_get_string_member_with_default(ev, "kind", "chat");
        char *line = g_strdup_printf("%s%s", strcmp(kind, "system") ? "" : "· ",
                                     json_object_get_string_member_with_default(ev, "text", ""));
        text_append(A->chat_view, line, CHAT_MAX_LINES);
        g_free(line);
        if (A->settings->listen_chat) {
            char *who = NULL, *order = owner_order(ev, &who);
            if (order)
                agent_submit(A->agent, order, who);
            g_free(order);
            g_free(who);
        }
        return;
    }
    if (!strcmp(name, "spawn")) {
        A->connected = A->spawned = TRUE;
        A->connecting = FALSE;
        update_title();
        start_status_timer();
        toast("In game");
    } else if (!strcmp(name, "disconnected")) {
        const char *why = json_object_get_string_member_with_default(ev, "reason", "");
        A->connected = A->spawned = A->connecting = FALSE;
        update_title();
        toast("Disconnected: %s", why);
    } else if (!strcmp(name, "death")) {
        toast("The bot died");
    }
    /* everything else goes to the log */
    JsonObject *copy = json_object_new();
    JsonObjectIter it;
    const char *k;
    JsonNode *v;
    json_object_iter_init(&it, ev);
    while (json_object_iter_next(&it, &k, &v))
        if (strcmp(k, "event"))
            json_object_set_member(copy, k, json_node_copy(v));
    JsonNode *n = json_node_init_object(json_node_alloc(), copy);
    char *s = json_to_string(n, FALSE);
    char *line = g_strdup_printf("[%s] %s", name, s);
    log_line(line);
    g_free(line);
    g_free(s);
    json_node_unref(n);
    json_object_unref(copy);
}

static void on_core_log(const char *line, gpointer ud)
{
    (void)ud;
    if (A)
        log_line(line);
}

/* ---- preferences ---- */

static GtkWidget *entry_row(const char *title, const char *value)
{
    GtkWidget *r = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r), title);
    gtk_editable_set_text(GTK_EDITABLE(r), value ? value : "");
    return r;
}

static GtkWidget *password_row(const char *title, const char *value)
{
    GtkWidget *r = adw_password_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r), title);
    gtk_editable_set_text(GTK_EDITABLE(r), value ? value : "");
    return r;
}

static const LlmProvider *selected_provider(void)
{
    guint i = adw_combo_row_get_selected(ADW_COMBO_ROW(A->p_provider));
    return i < (guint)llm_n_providers ? &llm_providers[i] : &llm_providers[0];
}

static void describe_provider_rows(const LlmProvider *p)
{
    char *t = g_strdup_printf("Model (e.g. %s)", p->model_hint);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(A->p_model), t);
    g_free(t);
    t = *p->base_url ? g_strdup_printf("API address (empty: %s)", p->base_url)
                     : g_strdup("API address (required, e.g. http://127.0.0.1:1234/v1)");
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(A->p_url), t);
    g_free(t);
    t = p->needs_key ? g_strdup_printf("API key (or %s)", p->key_env) : g_strdup("API key (optional)");
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(A->p_key), t);
    g_free(t);
}

static void provider_changed(GObject *row, GParamSpec *ps, gpointer ud)
{
    (void)row, (void)ps, (void)ud;
    const LlmProvider *p = selected_provider();
    if (g_strcmp0(p->id, A->p_last_provider)) {
        /* each provider keeps its own key */
        char *key = settings_lookup_api_key(p->id);
        gtk_editable_set_text(GTK_EDITABLE(A->p_key), key);
        memset(key, 0, strlen(key));
        g_free(key);
        g_free(A->p_last_provider);
        A->p_last_provider = g_strdup(p->id);
    }
    describe_provider_rows(p);
}

static void replace(char **dst, const char *src)
{
    if (*dst && *dst != src)
        memset(*dst, 0, strlen(*dst));
    g_free(*dst);
    *dst = g_strstrip(g_strdup(src ? src : ""));
}

static void prefs_closed(AdwDialog *d, gpointer ud)
{
    (void)d, (void)ud;
    Settings *s = A->settings;
    replace(&s->host, gtk_editable_get_text(GTK_EDITABLE(A->p_host)));
    s->port = (int)adw_spin_row_get_value(ADW_SPIN_ROW(A->p_port));
    replace(&s->username, gtk_editable_get_text(GTK_EDITABLE(A->p_user)));
    g_free(s->password);
    s->password = g_strdup(gtk_editable_get_text(GTK_EDITABLE(A->p_pass)));
    replace(&s->provider, selected_provider()->id);
    replace(&s->model, gtk_editable_get_text(GTK_EDITABLE(A->p_model)));
    g_free(s->api_key);
    s->api_key = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(A->p_key))));
    replace(&s->base_url, gtk_editable_get_text(GTK_EDITABLE(A->p_url)));
    replace(&s->owners, gtk_editable_get_text(GTK_EDITABLE(A->p_owners)));
    s->listen_chat = adw_switch_row_get_active(ADW_SWITCH_ROW(A->p_listen));
    s->max_steps = (int)adw_spin_row_get_value(ADW_SPIN_ROW(A->p_steps));
    if (!*s->username)
        replace(&s->username, "MineLLM");
    if (!*s->host)
        replace(&s->host, "127.0.0.1");
    char *why = NULL;
    if (!settings_save(s, &why)) {
        toast("%s", why);
        g_free(why);
    }
    g_clear_pointer(&A->p_last_provider, g_free);
    apply_settings();
}

static void show_prefs(void)
{
    Settings *s = A->settings;
    AdwPreferencesDialog *dlg = ADW_PREFERENCES_DIALOG(adw_preferences_dialog_new());

    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_title(page, "Server");
    adw_preferences_page_set_icon_name(page, "network-server-symbolic");
    AdwPreferencesGroup *g = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(g, "Minecraft server");
    adw_preferences_group_set_description(g, "Minecraft Java 26.3 (protocol 777), offline-mode accounts. The password "
                                             "answers login/register prompts (AuthMe and similar) and is kept in the keyring.");
    A->p_host = entry_row("Address", s->host);
    A->p_port = adw_spin_row_new_with_range(1, 65535, 1);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(A->p_port), "Port");
    adw_spin_row_set_value(ADW_SPIN_ROW(A->p_port), s->port);
    A->p_user = entry_row("Player name", s->username);
    A->p_pass = password_row("Login password", s->password);
    adw_preferences_group_add(g, A->p_host);
    adw_preferences_group_add(g, A->p_port);
    adw_preferences_group_add(g, A->p_user);
    adw_preferences_group_add(g, A->p_pass);
    adw_preferences_page_add(page, g);
    adw_preferences_dialog_add(dlg, page);

    page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_title(page, "Model");
    adw_preferences_page_set_icon_name(page, "applications-science-symbolic");
    g = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(g, "Language model");
    adw_preferences_group_set_description(g, "The model needs tool (function) calling. API keys are kept in the keyring.");
    GtkStringList *names = gtk_string_list_new(NULL);
    guint sel = 0;
    for (int k = 0; k < llm_n_providers; k++) {
        gtk_string_list_append(names, llm_providers[k].label);
        if (!g_strcmp0(llm_providers[k].id, s->provider))
            sel = k;
    }
    A->p_provider = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(A->p_provider), "Provider");
    adw_combo_row_set_model(ADW_COMBO_ROW(A->p_provider), G_LIST_MODEL(names));
    adw_combo_row_set_selected(ADW_COMBO_ROW(A->p_provider), sel);
    A->p_model = entry_row("Model", s->model);
    A->p_key = password_row("API key", s->api_key);
    A->p_url = entry_row("API address", s->base_url);
    A->p_last_provider = g_strdup(llm_providers[sel].id);
    describe_provider_rows(&llm_providers[sel]);
    g_signal_connect(A->p_provider, "notify::selected", G_CALLBACK(provider_changed), NULL);
    adw_preferences_group_add(g, A->p_provider);
    adw_preferences_group_add(g, A->p_model);
    adw_preferences_group_add(g, A->p_key);
    adw_preferences_group_add(g, A->p_url);
    adw_preferences_page_add(page, g);
    adw_preferences_dialog_add(dlg, page);

    page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_title(page, "Agent");
    adw_preferences_page_set_icon_name(page, "system-run-symbolic");
    g = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(g, "Orders in game chat");
    adw_preferences_group_set_description(
        g, "Owners can give the bot orders in the game: whisper to it, name it, or start the message with \"!\". "
           "On offline-mode servers names can be faked, so only use this where players must log in.");
    A->p_owners = entry_row("Owners (comma-separated player names)", s->owners);
    A->p_listen = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(A->p_listen), "Take orders from owners in chat");
    adw_switch_row_set_active(ADW_SWITCH_ROW(A->p_listen), s->listen_chat);
    adw_preferences_group_add(g, A->p_owners);
    adw_preferences_group_add(g, A->p_listen);
    adw_preferences_page_add(page, g);
    g = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(g, "Limits");
    A->p_steps = adw_spin_row_new_with_range(5, 500, 5);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(A->p_steps), "Tool calls per request");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(A->p_steps), "Then the model has to wrap up");
    adw_spin_row_set_value(ADW_SPIN_ROW(A->p_steps), s->max_steps);
    adw_preferences_group_add(g, A->p_steps);
    adw_preferences_page_add(page, g);
    adw_preferences_dialog_add(dlg, page);

    g_signal_connect(dlg, "closed", G_CALLBACK(prefs_closed), NULL);
    adw_dialog_present(ADW_DIALOG(dlg), GTK_WIDGET(A->win));
}

/* ---- actions ---- */

static void act_prefs(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a, (void)p, (void)ud;
    show_prefs();
}

static void act_reset(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a, (void)p, (void)ud;
    agent_reset(A->agent);
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(A->convo_box)))
        gtk_box_remove(GTK_BOX(A->convo_box), c);
}

static void act_about(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a, (void)p, (void)ud;
    AdwDialog *d = adw_about_dialog_new();
    adw_about_dialog_set_application_name(ADW_ABOUT_DIALOG(d), "MineLLM");
    adw_about_dialog_set_application_icon(ADW_ABOUT_DIALOG(d), "io.github.gubir34.MineLLM");
    adw_about_dialog_set_version(ADW_ABOUT_DIALOG(d), MINELLM_VERSION);
    adw_about_dialog_set_developer_name(ADW_ABOUT_DIALOG(d), "Gubir34");
    adw_about_dialog_set_comments(ADW_ABOUT_DIALOG(d),
                                  "A Minecraft player driven by a language model, on the native MineMCP core.");
    adw_about_dialog_set_website(ADW_ABOUT_DIALOG(d), "https://github.com/Gubir34/MineLLM");
    adw_about_dialog_set_license_type(ADW_ABOUT_DIALOG(d), GTK_LICENSE_MIT_X11);
    adw_dialog_present(d, GTK_WIDGET(A->win));
}

static void stop_clicked(GtkButton *b, gpointer ud)
{
    (void)b, (void)ud;
    agent_stop(A->agent);
}

static void send_clicked(GtkButton *b, gpointer ud)
{
    (void)b, (void)ud;
    submit_prompt();
}

static void entry_activate(GtkEntry *e, gpointer ud)
{
    (void)e, (void)ud;
    submit_prompt();
}

static void banner_clicked(AdwBanner *b, gpointer ud)
{
    (void)b, (void)ud;
    show_prefs();
}

/* ---- building the window ---- */

static GtkWidget *status_row(AdwPreferencesGroup *g, const char *title)
{
    GtkWidget *r = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r), title);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(r), "–");
    gtk_widget_add_css_class(r, "property");
    adw_preferences_group_add(g, r);
    return r;
}

static GtkWidget *build_side(void)
{
    AdwViewStack *stack = ADW_VIEW_STACK(adw_view_stack_new());

    GtkWidget *game = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(game, 12);
    gtk_widget_set_margin_end(game, 12);
    gtk_widget_set_margin_top(game, 12);
    gtk_widget_set_margin_bottom(game, 12);
    AdwPreferencesGroup *g = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(g, "Bot");
    A->row_health = status_row(g, "Health");
    A->row_food = status_row(g, "Food");
    A->row_pos = status_row(g, "Position");
    A->row_dim = status_row(g, "Dimension");
    A->row_task = status_row(g, "Task");
    A->row_reflex = status_row(g, "Reflexes");
    gtk_box_append(GTK_BOX(game), GTK_WIDGET(g));
    GtkWidget *chat_title = gtk_label_new("Game chat");
    gtk_widget_add_css_class(chat_title, "heading");
    gtk_label_set_xalign(GTK_LABEL(chat_title), 0);
    gtk_box_append(GTK_BOX(game), chat_title);
    A->chat_view = GTK_TEXT_VIEW(view_text(FALSE));
    GtkWidget *cs = scrolled(GTK_WIDGET(A->chat_view));
    gtk_widget_add_css_class(cs, "card");
    gtk_box_append(GTK_BOX(game), cs);
    A->chat_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(A->chat_entry), "Say something in game (/ for commands)");
    g_signal_connect(A->chat_entry, "activate", G_CALLBACK(chat_send), NULL);
    gtk_box_append(GTK_BOX(game), A->chat_entry);
    adw_view_stack_add_titled_with_icon(stack, scrolled(game), "game", "Game", "input-gaming-symbolic");

    A->log_view = GTK_TEXT_VIEW(view_text(TRUE));
    gtk_widget_add_css_class(GTK_WIDGET(A->log_view), "log");
    adw_view_stack_add_titled_with_icon(stack, scrolled(GTK_WIDGET(A->log_view)), "log", "Log",
                                        "utilities-terminal-symbolic");

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *sw = adw_view_switcher_new();
    adw_view_switcher_set_stack(ADW_VIEW_SWITCHER(sw), stack);
    adw_view_switcher_set_policy(ADW_VIEW_SWITCHER(sw), ADW_VIEW_SWITCHER_POLICY_WIDE);
    gtk_widget_set_margin_top(sw, 6);
    gtk_widget_set_margin_bottom(sw, 0);
    gtk_box_append(GTK_BOX(box), sw);
    gtk_widget_set_vexpand(GTK_WIDGET(stack), TRUE);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(stack));
    return box;
}

static GtkWidget *build_convo(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    A->banner = ADW_BANNER(adw_banner_new(""));
    adw_banner_set_button_label(A->banner, "Preferences");
    g_signal_connect(A->banner, "button-clicked", G_CALLBACK(banner_clicked), NULL);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(A->banner));

    A->convo_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(A->convo_box, 16);
    gtk_widget_set_margin_end(A->convo_box, 16);
    gtk_widget_set_margin_top(A->convo_box, 16);
    gtk_widget_set_margin_bottom(A->convo_box, 16);
    GtkWidget *clamp = adw_clamp_new();
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 900);
    adw_clamp_set_child(ADW_CLAMP(clamp), A->convo_box);
    A->convo_scroll = scrolled(clamp);
    gtk_box_append(GTK_BOX(box), A->convo_scroll);

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(bar, 12);
    gtk_widget_set_margin_end(bar, 12);
    gtk_widget_set_margin_top(bar, 6);
    gtk_widget_set_margin_bottom(bar, 12);
    A->entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(A->entry), "Tell the bot what to do, e.g. \"get 16 oak logs and make a crafting table\"");
    gtk_widget_set_hexpand(A->entry, TRUE);
    g_signal_connect(A->entry, "activate", G_CALLBACK(entry_activate), NULL);
    A->send_btn = gtk_button_new_from_icon_name("mail-send-symbolic");
    gtk_widget_set_tooltip_text(A->send_btn, "Send");
    gtk_widget_add_css_class(A->send_btn, "suggested-action");
    g_signal_connect(A->send_btn, "clicked", G_CALLBACK(send_clicked), NULL);
    A->stop_btn = gtk_button_new_from_icon_name("media-playback-stop-symbolic");
    gtk_widget_set_tooltip_text(A->stop_btn, "Stop the agent and the bot's task");
    gtk_widget_add_css_class(A->stop_btn, "destructive-action");
    gtk_widget_set_sensitive(A->stop_btn, FALSE);
    g_signal_connect(A->stop_btn, "clicked", G_CALLBACK(stop_clicked), NULL);
    gtk_box_append(GTK_BOX(bar), A->entry);
    gtk_box_append(GTK_BOX(bar), A->send_btn);
    gtk_box_append(GTK_BOX(bar), A->stop_btn);
    gtk_box_append(GTK_BOX(box), bar);
    return box;
}

static void load_css(void)
{
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_resource(css, "/io/github/gubir34/MineLLM/style.css");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

static void on_shutdown(GApplication *app, gpointer ud)
{
    (void)app, (void)ud;
    if (!A)
        return;
    if (A->status_timer)
        g_source_remove(A->status_timer);
    agent_free(A->agent);
    A->agent = NULL;
    App *a = A;
    A = NULL; /* callbacks fired while closing see no window */
    core_client_free(a->core);
    g_object_unref(a->http);
    settings_free(a->settings);
    g_free(a->player_name);
    g_free(a);
}

void minellm_window_activate(AdwApplication *app)
{
    if (A) {
        gtk_window_present(A->win);
        return;
    }
    A = g_new0(App, 1);
    A->app = app;
    A->settings = settings_load();
    A->http = soup_session_new();
    soup_session_set_timeout(A->http, 180);
    A->core = core_client_new(NULL, NULL, on_core_event, on_core_log, NULL);
    AgentUi ui = {on_agent_entry, on_agent_busy};
    A->agent = agent_new(A->core, A->http, &ui, NULL);
    load_css();

    static const GActionEntry actions[] = {
        {.name = "preferences", .activate = act_prefs},
        {.name = "reset", .activate = act_reset},
        {.name = "about", .activate = act_about},
    };
    g_action_map_add_action_entries(G_ACTION_MAP(app), actions, G_N_ELEMENTS(actions), NULL);
    const char *prefs_accels[] = {"<Control>comma", NULL};
    const char *reset_accels[] = {"<Control>n", NULL};
    gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.preferences", prefs_accels);
    gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.reset", reset_accels);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), NULL);

    GtkWidget *win = adw_application_window_new(GTK_APPLICATION(app));
    A->win = GTK_WINDOW(win);
    gtk_window_set_title(A->win, "MineLLM");
    gtk_window_set_default_size(A->win, 1200, 760);

    GtkWidget *header = adw_header_bar_new();
    A->title = ADW_WINDOW_TITLE(adw_window_title_new("MineLLM", ""));
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), GTK_WIDGET(A->title));
    A->connect_btn = gtk_button_new_with_label("Connect");
    g_signal_connect(A->connect_btn, "clicked", G_CALLBACK(connect_clicked), NULL);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), A->connect_btn);
    GMenu *menu = g_menu_new();
    g_menu_append(menu, "New Conversation", "app.reset");
    g_menu_append(menu, "Preferences", "app.preferences");
    g_menu_append(menu, "About MineLLM", "app.about");
    GtkWidget *mb = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(mb), "open-menu-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(mb), G_MENU_MODEL(menu));
    gtk_menu_button_set_primary(GTK_MENU_BUTTON(mb), TRUE);
    g_object_unref(menu);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), mb);
    A->spinner = adw_spinner_new();
    gtk_widget_set_visible(A->spinner, FALSE);
    gtk_widget_set_tooltip_text(A->spinner, "The agent is working");
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), A->spinner);

    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_start_child(GTK_PANED(paned), build_convo());
    gtk_paned_set_end_child(GTK_PANED(paned), build_side());
    gtk_paned_set_resize_end_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_position(GTK_PANED(paned), 780);

    GtkWidget *tv = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tv), paned);
    A->toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    adw_toast_overlay_set_child(A->toasts, tv);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(win), GTK_WIDGET(A->toasts));

    update_title();
    apply_settings();
    char *msg = g_strdup_printf("MineLLM %s: %d tools, core %s", MINELLM_VERSION, agent_tool_count(A->agent),
                                core_client_socket_path(A->core));
    log_line(msg);
    g_free(msg);
    on_agent_entry(AGENT_INFO, NULL,
                   "Connect the bot, then tell it what to do. It works through the same tools MineMCP gives MCP "
                   "clients: moving, mining, crafting, fighting, farming, trading and more.",
                   TRUE, NULL);
    gtk_window_present(A->win);
    gtk_widget_grab_focus(A->entry);
}
