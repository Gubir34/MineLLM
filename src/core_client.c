#include "core_client.h"

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <signal.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>

#define START_TIMEOUT_US (5 * G_USEC_PER_SEC)
#define RETRY_MS 100

enum state { ST_IDLE, ST_STARTING, ST_READY, ST_STOPPING };

typedef struct {
    char *cmd;
    JsonObject *args; /* owned copy, may be NULL */
    int timeout_s;
    gboolean task_wait;
    CoreReplyFn cb;
    gpointer ud;
} Queued;

typedef struct {
    CoreClient *c;
    gint64 id; /* request id, or task id for a task wait */
    gboolean task_wait;
    int timeout_s;
    guint timer;
    CoreReplyFn cb;
    gpointer ud;
} Pending;

struct CoreClient {
    char *binary, *socket_path;
    char *password, *spawned_password;
    CoreEventFn on_event;
    CoreLogFn on_log;
    gpointer ud;

    enum state state;
    GSubprocess *child;
    GSocketConnection *conn;
    GDataInputStream *in;
    GOutputStream *out;
    GCancellable *cancel; /* the current connection's reads */
    GQueue queue;         /* Queued, until the connection is up */
    GHashTable *pending;  /* request id -> Pending */
    GHashTable *tasks;    /* task id -> Pending */
    GHashTable *early;    /* task id -> JsonObject: task_done seen before its reply */
    gint64 next_id;
    gint64 start_deadline;
    guint retry_source;
    GCancellable *life; /* cancelled when the client is freed */
};

static void logf_(CoreClient *c, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void logf_(CoreClient *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *s = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    if (c->on_log)
        c->on_log(s, c->ud);
    g_free(s);
}

char *core_default_socket_path(void)
{
    const char *dir = g_get_user_runtime_dir();
    return g_strdup_printf("%s/minellm-%u.sock", dir, (unsigned)getuid());
}

char *core_default_binary(void)
{
    char *self = g_file_read_link("/proc/self/exe", NULL);
    if (self) {
        char *dir = g_path_get_dirname(self);
        char *cand = g_build_filename(dir, "minemcp", NULL);
        g_free(dir);
        g_free(self);
        if (g_file_test(cand, G_FILE_TEST_IS_EXECUTABLE))
            return cand;
        g_free(cand);
    }
    char *found = g_find_program_in_path("minemcp");
    return found ? found : g_strdup("minemcp");
}

static void queued_free(Queued *q)
{
    g_free(q->cmd);
    if (q->args)
        json_object_unref(q->args);
    g_free(q);
}

static void pending_free(gpointer p)
{
    Pending *pd = p;
    if (pd->timer)
        g_source_remove(pd->timer);
    g_free(pd);
}

CoreClient *core_client_new(const char *binary, const char *socket_path, CoreEventFn on_event, CoreLogFn on_log,
                            gpointer user_data)
{
    CoreClient *c = g_new0(CoreClient, 1);
    c->binary = binary ? g_strdup(binary) : core_default_binary();
    c->socket_path = socket_path ? g_strdup(socket_path) : core_default_socket_path();
    c->on_event = on_event;
    c->on_log = on_log;
    c->ud = user_data;
    g_queue_init(&c->queue);
    c->pending = g_hash_table_new_full(g_int64_hash, g_int64_equal, NULL, pending_free);
    c->tasks = g_hash_table_new_full(g_int64_hash, g_int64_equal, NULL, pending_free);
    c->early = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, (GDestroyNotify)json_object_unref);
    c->next_id = 1;
    c->life = g_cancellable_new();
    return c;
}

const char *core_client_socket_path(CoreClient *c)
{
    return c->socket_path;
}

void core_client_set_password(CoreClient *c, const char *password)
{
    g_free(c->password);
    c->password = password && *password ? g_strdup(password) : NULL;
}

gboolean core_client_running_with(CoreClient *c, const char *password)
{
    if (c->state != ST_READY || !c->child)
        return FALSE;
    const char *a = c->spawned_password ? c->spawned_password : "", *b = password ? password : "";
    return strcmp(a, b) == 0;
}

/* ---- failing everything that waits ---- */

static void fail_all(CoreClient *c, const char *why)
{
    GList *l;
    while ((l = g_queue_peek_head_link(&c->queue))) {
        Queued *q = g_queue_pop_head(&c->queue);
        q->cb(NULL, why, q->ud);
        queued_free(q);
    }
    GHashTable *tabs[2] = {c->pending, c->tasks};
    for (int k = 0; k < 2; k++) {
        GList *vals = g_hash_table_get_values(tabs[k]);
        /* steal first: a callback may send new requests */
        GPtrArray *todo = g_ptr_array_new();
        for (GList *v = vals; v; v = v->next)
            g_ptr_array_add(todo, v->data);
        g_list_free(vals);
        g_hash_table_steal_all(tabs[k]);
        for (guint i = 0; i < todo->len; i++) {
            Pending *pd = todo->pdata[i];
            CoreReplyFn cb = pd->cb;
            gpointer ud = pd->ud;
            pending_free(pd);
            cb(NULL, why, ud);
        }
        g_ptr_array_free(todo, TRUE);
    }
    g_hash_table_remove_all(c->early);
}

static void drop_connection(CoreClient *c, const char *why)
{
    if (c->cancel) {
        g_cancellable_cancel(c->cancel);
        g_clear_object(&c->cancel);
    }
    g_clear_object(&c->in);
    c->out = NULL;
    if (c->conn) {
        g_io_stream_close(G_IO_STREAM(c->conn), NULL, NULL);
        g_clear_object(&c->conn);
    }
    if (c->state == ST_READY || c->state == ST_STARTING)
        c->state = ST_IDLE;
    fail_all(c, why);
}

/* ---- writing ---- */

static gboolean write_request(CoreClient *c, gint64 id, const char *cmd, JsonObject *args, GError **err)
{
    JsonObject *o = json_object_new();
    if (args) {
        JsonObjectIter it;
        const char *name;
        JsonNode *val;
        json_object_iter_init(&it, args);
        while (json_object_iter_next(&it, &name, &val))
            json_object_set_member(o, name, json_node_copy(val));
    }
    json_object_set_int_member(o, "id", id);
    json_object_set_string_member(o, "cmd", cmd);
    JsonNode *n = json_node_init_object(json_node_alloc(), o);
    char *s = json_to_string(n, FALSE);
    char *line = g_strconcat(s, "\n", NULL);
    gboolean ok = g_output_stream_write_all(c->out, line, strlen(line), NULL, NULL, err);
    g_free(line);
    g_free(s);
    json_node_unref(n);
    json_object_unref(o);
    return ok;
}

static gboolean request_timeout(gpointer p)
{
    Pending *pd = p;
    CoreClient *c = pd->c;
    pd->timer = 0;
    CoreReplyFn cb = pd->cb;
    gpointer ud = pd->ud;
    int t = pd->timeout_s;
    g_hash_table_remove(c->pending, &pd->id);
    char *msg = g_strdup_printf("the core did not answer within %d s", t);
    cb(NULL, msg, ud);
    g_free(msg);
    return G_SOURCE_REMOVE;
}

static void stop_reply(JsonObject *r, const char *e, gpointer ud)
{
    (void)r, (void)e, (void)ud;
}

static gboolean task_timeout(gpointer p)
{
    Pending *pd = p;
    CoreClient *c = pd->c;
    pd->timer = 0;
    CoreReplyFn cb = pd->cb;
    gpointer ud = pd->ud;
    int t = pd->timeout_s;
    g_hash_table_remove(c->tasks, &pd->id);
    core_client_request(c, "stop", NULL, 10, stop_reply, NULL);
    char *msg = g_strdup_printf("no result after %d s; task stopped", t);
    cb(NULL, msg, ud);
    g_free(msg);
    return G_SOURCE_REMOVE;
}

static void send_now(CoreClient *c, Queued *q)
{
    gint64 id = c->next_id++;
    GError *err = NULL;
    if (!write_request(c, id, q->cmd, q->args, &err)) {
        char *msg = g_strdup_printf("cannot write to the core: %s", err->message);
        g_error_free(err);
        q->cb(NULL, msg, q->ud);
        g_free(msg);
        drop_connection(c, "connection to the core lost");
        return;
    }
    Pending *pd = g_new0(Pending, 1);
    pd->c = c;
    pd->id = id;
    pd->task_wait = q->task_wait;
    pd->timeout_s = q->timeout_s;
    pd->cb = q->cb;
    pd->ud = q->ud;
    /* the reply itself comes fast even for tasks; the task wait is separate */
    pd->timer = g_timeout_add_seconds(q->task_wait ? 30 : MAX(q->timeout_s, 1), request_timeout, pd);
    g_hash_table_insert(c->pending, &pd->id, pd);
}

static void flush_queue(CoreClient *c)
{
    while (c->state == ST_READY && !g_queue_is_empty(&c->queue)) {
        Queued *q = g_queue_pop_head(&c->queue);
        send_now(c, q);
        queued_free(q);
    }
}

/* ---- reading ---- */

static void handle_reply(CoreClient *c, JsonObject *o)
{
    gint64 id = json_object_get_int_member_with_default(o, "id", -1);
    Pending *pd = g_hash_table_lookup(c->pending, &id);
    if (!pd)
        return;
    g_hash_table_steal(c->pending, &id);
    if (pd->timer)
        g_source_remove(pd->timer);
    pd->timer = 0;
    if (!json_object_get_boolean_member_with_default(o, "ok", FALSE)) {
        const char *e = json_object_get_string_member_with_default(o, "error", "request failed");
        pd->cb(NULL, e, pd->ud);
        g_free(pd);
        return;
    }
    JsonNode *tn = json_object_get_member(o, "task");
    if (pd->task_wait && tn && JSON_NODE_HOLDS_VALUE(tn) && json_node_get_value_type(tn) == G_TYPE_INT64) {
        gint64 task = json_node_get_int(tn);
        JsonObject *done = g_hash_table_lookup(c->early, &task);
        if (done) {
            json_object_ref(done);
            g_hash_table_remove(c->early, &task);
            pd->cb(done, NULL, pd->ud);
            json_object_unref(done);
            g_free(pd);
            return;
        }
        pd->id = task;
        pd->timer = g_timeout_add_seconds(MAX(pd->timeout_s, 1), task_timeout, pd);
        g_hash_table_insert(c->tasks, &pd->id, pd);
        return;
    }
    pd->cb(o, NULL, pd->ud);
    g_free(pd);
}

static void handle_task_done(CoreClient *c, JsonObject *ev)
{
    gint64 task = json_object_get_int_member_with_default(ev, "task", -1);
    Pending *pd = g_hash_table_lookup(c->tasks, &task);
    if (!pd) {
        /* its reply may still be on the way; keep a few */
        if (g_hash_table_size(c->early) < 32) {
            gint64 *k = g_new(gint64, 1);
            *k = task;
            g_hash_table_insert(c->early, k, json_object_ref(ev));
        }
        return;
    }
    g_hash_table_steal(c->tasks, &task);
    if (pd->timer)
        g_source_remove(pd->timer);
    pd->cb(ev, NULL, pd->ud);
    g_free(pd);
}

static void read_next(CoreClient *c);

static void on_line(GObject *src, GAsyncResult *res, gpointer ud)
{
    GError *err = NULL;
    gsize len = 0;
    char *line = g_data_input_stream_read_line_finish_utf8(G_DATA_INPUT_STREAM(src), res, &len, &err);
    if (err && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(err);
        return; /* the client may be gone */
    }
    CoreClient *c = ud;
    if (!line) {
        logf_(c, "[minellm] control connection to the core closed%s%s", err ? ": " : "", err ? err->message : "");
        if (err)
            g_error_free(err);
        drop_connection(c, "connection to the core closed");
        return;
    }
    if (len) {
        JsonNode *n = json_from_string(line, &err);
        if (!n || !JSON_NODE_HOLDS_OBJECT(n)) {
            logf_(c, "[minellm] bad line from the core: %.200s", line);
            g_clear_error(&err);
        } else {
            JsonObject *o = json_node_get_object(n);
            json_object_ref(o);
            if (json_object_has_member(o, "event")) {
                if (!g_strcmp0(json_object_get_string_member_with_default(o, "event", ""), "task_done"))
                    handle_task_done(c, o);
                if (c->on_event)
                    c->on_event(o, c->ud);
            } else {
                handle_reply(c, o);
            }
            json_object_unref(o);
        }
        if (n)
            json_node_unref(n);
    }
    g_free(line);
    if (c->in)
        read_next(c);
}

static void read_next(CoreClient *c)
{
    g_data_input_stream_read_line_async(c->in, G_PRIORITY_DEFAULT, c->cancel, on_line, c);
}

/* ---- starting ---- */

static void try_connect(CoreClient *c);

static void on_stderr_line(GObject *src, GAsyncResult *res, gpointer ud)
{
    GError *err = NULL;
    char *line = g_data_input_stream_read_line_finish_utf8(G_DATA_INPUT_STREAM(src), res, NULL, &err);
    if (!line) {
        g_clear_error(&err);
        g_object_unref(src);
        return; /* end of the pipe, or the client is gone */
    }
    CoreClient *c = ud;
    logf_(c, "[core] %s", line);
    g_free(line);
    g_data_input_stream_read_line_async(G_DATA_INPUT_STREAM(src), G_PRIORITY_LOW, c->life, on_stderr_line, c);
}

static void on_child_exit(GObject *src, GAsyncResult *res, gpointer ud)
{
    GSubprocess *child = G_SUBPROCESS(src);
    GError *err = NULL;
    if (!g_subprocess_wait_finish(child, res, &err) && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(err);
        g_object_unref(child);
        return; /* the client is gone */
    }
    g_clear_error(&err);
    CoreClient *c = ud;
    if (c->child == child) {
        logf_(c, "[minellm] the core exited (%s %d)", g_subprocess_get_if_signaled(child) ? "signal" : "status",
              g_subprocess_get_if_signaled(child) ? g_subprocess_get_term_sig(child) : g_subprocess_get_exit_status(child));
        g_clear_object(&c->child);
        g_clear_pointer(&c->spawned_password, g_free);
        if (c->state == ST_STOPPING)
            c->state = ST_IDLE;
        if (!g_queue_is_empty(&c->queue))
            try_connect(c);
    }
    g_object_unref(child);
}

/* In the child before exec: the core goes away with us, even if we crash. */
static void child_setup(gpointer ud)
{
    (void)ud;
    prctl(PR_SET_PDEATHSIG, SIGTERM);
}

static gboolean spawn_child(CoreClient *c, GError **err)
{
    GSubprocessLauncher *l = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
    g_subprocess_launcher_set_child_setup(l, child_setup, NULL, NULL);
    if (c->password)
        g_subprocess_launcher_setenv(l, "MC_LOGIN_PASSWORD", c->password, TRUE);
    else
        g_subprocess_launcher_unsetenv(l, "MC_LOGIN_PASSWORD");
    logf_(c, "[minellm] starting %s --socket %s", c->binary, c->socket_path);
    GSubprocess *child = g_subprocess_launcher_spawn(l, err, c->binary, "--socket", c->socket_path, NULL);
    g_object_unref(l);
    if (!child)
        return FALSE;
    c->child = child;
    c->spawned_password = g_strdup(c->password ? c->password : "");
    GDataInputStream *errs = g_data_input_stream_new(g_subprocess_get_stderr_pipe(child));
    g_data_input_stream_read_line_async(errs, G_PRIORITY_LOW, c->life, on_stderr_line, c);
    g_subprocess_wait_async(child, c->life, on_child_exit, c);
    g_object_ref(child); /* held by the wait */
    return TRUE;
}

static gboolean retry_cb(gpointer p)
{
    CoreClient *c = p;
    c->retry_source = 0;
    try_connect(c);
    return G_SOURCE_REMOVE;
}

static void on_connected(GObject *src, GAsyncResult *res, gpointer ud)
{
    GError *err = NULL;
    GSocketConnection *conn = g_socket_client_connect_finish(G_SOCKET_CLIENT(src), res, &err);
    g_object_unref(src);
    if (err && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(err);
        return; /* the client is gone */
    }
    CoreClient *c = ud;
    if (!conn) {
        if (!c->child && c->state == ST_STARTING) {
            GError *serr = NULL;
            if (!spawn_child(c, &serr)) {
                char *msg = g_strdup_printf("cannot start the core (%s): %s", c->binary, serr->message);
                logf_(c, "[minellm] %s", msg);
                g_error_free(serr);
                c->state = ST_IDLE;
                fail_all(c, msg);
                g_free(msg);
                g_error_free(err);
                return;
            }
        }
        if (g_get_monotonic_time() > c->start_deadline) {
            char *msg = g_strdup_printf("the core did not open %s: %s", c->socket_path, err->message);
            logf_(c, "[minellm] %s", msg);
            c->state = ST_IDLE;
            fail_all(c, msg);
            g_free(msg);
        } else {
            c->retry_source = g_timeout_add(RETRY_MS, retry_cb, c);
        }
        g_error_free(err);
        return;
    }
    c->conn = conn;
    c->cancel = g_cancellable_new();
    c->in = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(conn)));
    g_data_input_stream_set_newline_type(c->in, G_DATA_STREAM_NEWLINE_TYPE_LF);
    c->out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
    c->state = ST_READY;
    read_next(c);
    flush_queue(c);
}

static void try_connect(CoreClient *c)
{
    if (c->state == ST_READY) {
        flush_queue(c);
        return;
    }
    if (c->state == ST_STOPPING || c->retry_source)
        return;
    if (c->state == ST_IDLE) {
        c->state = ST_STARTING;
        c->start_deadline = g_get_monotonic_time() + START_TIMEOUT_US;
    }
    GSocketClient *sc = g_socket_client_new();
    GSocketAddress *addr = g_unix_socket_address_new(c->socket_path);
    g_socket_client_connect_async(sc, G_SOCKET_CONNECTABLE(addr), c->life, on_connected, c);
    g_object_unref(addr);
}

static void enqueue(CoreClient *c, const char *cmd, JsonObject *args, int timeout_s, gboolean task_wait, CoreReplyFn cb,
                    gpointer ud)
{
    Queued *q = g_new0(Queued, 1);
    q->cmd = g_strdup(cmd);
    if (args) {
        q->args = json_object_new();
        JsonObjectIter it;
        const char *name;
        JsonNode *val;
        json_object_iter_init(&it, args);
        while (json_object_iter_next(&it, &name, &val))
            json_object_set_member(q->args, name, json_node_copy(val));
    }
    q->timeout_s = timeout_s;
    q->task_wait = task_wait;
    q->cb = cb;
    q->ud = ud;
    if (c->state == ST_READY) {
        send_now(c, q);
        queued_free(q);
        return;
    }
    g_queue_push_tail(&c->queue, q);
    if (c->state == ST_IDLE)
        try_connect(c);
}

void core_client_request(CoreClient *c, const char *cmd, JsonObject *args, int timeout_s, CoreReplyFn cb, gpointer ud)
{
    enqueue(c, cmd, args, timeout_s, FALSE, cb, ud);
}

void core_client_run(CoreClient *c, const char *cmd, JsonObject *args, int timeout_s, CoreReplyFn cb, gpointer ud)
{
    enqueue(c, cmd, args, timeout_s, TRUE, cb, ud);
}

/* ---- stopping ---- */

static gboolean force_exit(gpointer p)
{
    GSubprocess *child = p;
    if (!g_subprocess_get_identifier(child)) {
        g_object_unref(child);
        return G_SOURCE_REMOVE;
    }
    g_subprocess_force_exit(child);
    g_object_unref(child);
    return G_SOURCE_REMOVE;
}

static void shutdown_core(CoreClient *c, gboolean keep_client)
{
    if (c->retry_source) {
        g_source_remove(c->retry_source);
        c->retry_source = 0;
    }
    if (c->state == ST_READY && c->out) {
        GError *err = NULL;
        if (!write_request(c, c->next_id++, "shutdown", NULL, &err))
            g_clear_error(&err);
    }
    drop_connection(c, "the core was stopped");
    if (c->child) {
        g_timeout_add(1500, force_exit, g_object_ref(c->child));
        c->state = keep_client ? ST_STOPPING : ST_IDLE;
    } else {
        c->state = ST_IDLE;
    }
}

void core_client_restart(CoreClient *c)
{
    shutdown_core(c, TRUE);
}

void core_client_free(CoreClient *c)
{
    if (!c)
        return;
    shutdown_core(c, FALSE);
    g_cancellable_cancel(c->life); /* no callback touches the freed client */
    g_clear_object(&c->life);
    g_clear_object(&c->child);
    g_hash_table_destroy(c->pending);
    g_hash_table_destroy(c->tasks);
    g_hash_table_destroy(c->early);
    g_free(c->binary);
    g_free(c->socket_path);
    g_free(c->password);
    g_free(c->spawned_password);
    g_free(c);
}
