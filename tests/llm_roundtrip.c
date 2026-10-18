/* Test: a tool call's extra_content (Gemini thought signatures) goes back to
 * the provider unchanged on the next request.
 *   llm_roundtrip BASE_URL   (a mock server; see tests/mock_gemini.py) */
#include "../src/llm.h"

#include <stdio.h>

static GMainLoop *loop;
static JsonArray *messages;
static int failures, round;
static LlmConfig cfg;
static SoupSession *http;

static void on_reply(JsonObject *a, const char *err, gpointer ud)
{
    (void)ud;
    if (!a) {
        printf("round %d: error %s\n", round, err);
        failures++;
        g_main_loop_quit(loop);
        return;
    }
    if (round == 1) {
        JsonArray *calls = json_object_get_array_member(a, "tool_calls");
        JsonObject *call = json_array_get_object_element(calls, 0);
        if (!json_object_has_member(call, "extra_content")) {
            printf("round 1: extra_content was dropped from the tool call\n");
            failures++;
        }
        json_array_add_object_element(messages, json_object_ref(a));
        JsonObject *t = json_object_new();
        json_object_set_string_member(t, "role", "tool");
        json_object_set_string_member(t, "tool_call_id", json_object_get_string_member(call, "id"));
        json_object_set_string_member(t, "content", "{\"slots\":[]}");
        json_array_add_object_element(messages, t);
        round = 2;
        llm_chat_async(http, &cfg, messages, NULL, NULL, on_reply, NULL);
        return;
    }
    printf("round 2: %s\n", json_object_get_string_member_with_default(a, "content", "(none)"));
    g_main_loop_quit(loop);
}

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    loop = g_main_loop_new(NULL, FALSE);
    http = soup_session_new();
    cfg = (LlmConfig){.provider = "google", .model = "gemini-test", .api_key = "test", .base_url = argv[1]};
    messages = json_array_new();
    JsonObject *u = json_object_new();
    json_object_set_string_member(u, "role", "user");
    json_object_set_string_member(u, "content", "what is in the inventory?");
    json_array_add_object_element(messages, u);
    round = 1;
    llm_chat_async(http, &cfg, messages, NULL, NULL, on_reply, NULL);
    g_main_loop_run(loop);
    printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
