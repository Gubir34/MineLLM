/* Developer tool: runs the agent without the window.
 *   agent_probe HOST PORT USERNAME PROVIDER MODEL "request"
 * MC_LOGIN_PASSWORD and MC_REPLY_API_KEY / MC_REPLY_BASE_URL are read from the
 * environment. Prints the conversation to stdout and exits when the agent is
 * done. */
#include "../src/agent.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static GMainLoop *loop;
static Agent *agent;
static CoreClient *core;
static const char *request;
static gboolean started;

static void entry(AgentEntry k, const char *title, const char *text, gboolean ok, gpointer ud)
{
    (void)ud;
    static const char *const kinds[] = {"USER", "REPLY", "CALL", "RESULT", "ERROR", "INFO"};
    printf("[%s]%s%s%s %.*s\n", kinds[k], title ? " " : "", title ? title : "", k == AGENT_TOOL_RESULT && !ok ? " (failed)" : "",
           k == AGENT_TOOL_RESULT ? 400 : 4000, text);
    fflush(stdout);
}

static void busy(gboolean b, gpointer ud)
{
    (void)ud;
    if (!b && started)
        g_main_loop_quit(loop);
}

static void on_event(JsonObject *ev, gpointer ud)
{
    (void)ud;
    const char *name = json_object_get_string_member_with_default(ev, "event", "");
    if (!g_strcmp0(name, "spawn") && !started) {
        started = TRUE;
        g_usleep(1500 * 1000);
        agent_submit(agent, request, NULL);
    } else if (!g_strcmp0(name, "disconnected")) {
        printf("[disconnected] %s\n", json_object_get_string_member_with_default(ev, "reason", ""));
        if (!started)
            g_main_loop_quit(loop);
    }
}

static void on_log(const char *line, gpointer ud)
{
    (void)ud;
    if (g_getenv("PROBE_VERBOSE"))
        fprintf(stderr, "%s\n", line);
}

static void on_connect(JsonObject *r, const char *err, gpointer ud)
{
    (void)ud;
    if (!r) {
        fprintf(stderr, "connect failed: %s\n", err);
        g_main_loop_quit(loop);
    }
}

int main(int argc, char **argv)
{
    if (argc != 7) {
        fprintf(stderr, "usage: %s HOST PORT USERNAME PROVIDER MODEL REQUEST\n", argv[0]);
        return 2;
    }
    request = argv[6];
    loop = g_main_loop_new(NULL, FALSE);
    char *sock = g_strdup_printf("%s/minellm-probe-%d.sock", g_get_user_runtime_dir(), (int)getpid());
    core = core_client_new(NULL, sock, on_event, on_log, NULL);
    core_client_set_password(core, g_getenv("MC_LOGIN_PASSWORD"));
    SoupSession *http = soup_session_new();
    soup_session_set_timeout(http, 300);
    AgentUi ui = {entry, busy};
    agent = agent_new(core, http, &ui, NULL);
    LlmConfig c = {.provider = argv[4], .model = argv[5], .api_key = (char *)g_getenv("MC_REPLY_API_KEY"),
                   .base_url = (char *)g_getenv("MC_REPLY_BASE_URL"), .max_tokens = 4096};
    char *owners[] = {NULL};
    agent_configure(agent, &c, 25, argv[3], owners);
    printf("%d tools\n", agent_tool_count(agent));
    JsonObject *args = json_object_new();
    json_object_set_string_member(args, "host", argv[1]);
    json_object_set_int_member(args, "port", atoi(argv[2]));
    json_object_set_string_member(args, "username", argv[3]);
    core_client_request(core, "connect", args, 30, on_connect, NULL);
    json_object_unref(args);
    g_main_loop_run(loop);
    agent_free(agent);
    core_client_free(core);
    g_object_unref(http);
    g_free(sock);
    return 0;
}
