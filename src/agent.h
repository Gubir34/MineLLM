#ifndef MINELLM_AGENT_H
#define MINELLM_AGENT_H

#include "core_client.h"
#include "llm.h"

/* The agent: a conversation with the model, which acts in the game only
 * through the core's tools (the same set MineMCP offers to MCP clients).
 * Each tool call is run on the core and its outcome is handed back to the
 * model until it answers without calling a tool. */

typedef enum {
    AGENT_USER,        /* a request (title: who) */
    AGENT_REPLY,       /* the model's text */
    AGENT_TOOL_CALL,   /* title: tool name, text: arguments */
    AGENT_TOOL_RESULT, /* title: tool name, text: outcome, ok */
    AGENT_ERROR,
    AGENT_INFO,
} AgentEntry;

typedef struct {
    void (*entry)(AgentEntry kind, const char *title, const char *text, gboolean ok, gpointer ud);
    void (*busy)(gboolean busy, gpointer ud);
} AgentUi;

typedef struct Agent Agent;

Agent *agent_new(CoreClient *core, SoupSession *http, const AgentUi *ui, gpointer ud);
void agent_free(Agent *a);
/* Model, step limit, bot name and owners (lowercase names, copied). */
void agent_configure(Agent *a, const LlmConfig *llm, int max_steps, const char *bot_name, char **owners);
/* A request from the app's user (player == NULL) or an owner in game chat. */
void agent_submit(Agent *a, const char *text, const char *player);
/* Cancels the model call and the running tool, and ends the turn. */
void agent_stop(Agent *a);
/* Forgets the conversation. */
void agent_reset(Agent *a);
gboolean agent_busy(Agent *a);
int agent_tool_count(Agent *a);

#endif
