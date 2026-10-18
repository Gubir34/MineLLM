# MineLLM

A Minecraft player driven by a language model, as a native Linux desktop app (GTK 4 + libadwaita).

You tell the bot what to do in plain language: "get 16 oak logs and make a crafting table",
"mine down to diamond level and bring back iron", "follow me". A model of your choice plans the
work and carries it out through the bot's tools, one step at a time, checking every result. Owners
can also give the bot orders from inside the game chat.

The bot itself is the [MineMCP](https://github.com/Gubir34/MineMCP) core, a Minecraft Java 26.3
client written in C (pathfinding, mining, building, crafting, combat, farming, trading, travel,
reflexes). MineMCP exposes that core to MCP clients such as Claude Desktop; MineLLM instead puts the
model loop in the app itself, so no MCP client is needed.

## Model providers

| Provider | Notes |
|---|---|
| Ollama | local models, no key; uses Ollama's own API with a 16k context so all tool definitions fit |
| OpenAI (ChatGPT) | `OPENAI_API_KEY` or the key field |
| Anthropic (Claude) | `ANTHROPIC_API_KEY` or the key field |
| Google AI (Gemini) | `GEMINI_API_KEY` / `GOOGLE_API_KEY` or the key field; with no model set it uses `gemini-flash-lite-latest`, which the free tier allows the most requests a day |
| OpenRouter | `OPENROUTER_API_KEY` or the key field |
| Groq | `GROQ_API_KEY` or the key field |
| OpenAI-compatible server | LM Studio, vLLM, llama.cpp server...: give the address ending in `/v1` |

The model must support tool (function) calling. Small local models work for simple requests
(Qwen 3/3.5 with 4B+ parameters handled "collect 3 dirt and count it" fine); bigger models plan
long jobs much better.

## Building

Dependencies (Arch: `gtk4 libadwaita json-glib libsoup3 libsecret zlib cmake ninja`;
Debian/Ubuntu: `libgtk-4-dev libadwaita-1-dev libjson-glib-dev libsoup-3.0-dev libsecret-1-dev
zlib1g-dev cmake ninja-build`). libadwaita 1.6 or newer is needed.

```sh
git clone --recursive https://github.com/Gubir34/MineLLM.git
cd MineLLM
cmake -S . -B build -G Ninja
cmake --build build
./build/minellm
```

`cmake --install build --prefix ~/.local` installs `minellm`, the `minemcp` core, the desktop
entry and the icon. If you cloned without `--recursive`, run `git submodule update --init` first.

## Using it

1. Open Preferences (`Ctrl+,`): the server address, port and player name (offline-mode accounts),
   the login password for AuthMe-style `/login` prompts, and the model provider, model and key.
2. Press Connect. The panel on the right shows health, food, position, the running task and the
   game chat; the Log tab shows what the core is doing.
3. Type a request. Every tool call and its outcome appears in the conversation (click a result to
   see all of it). The stop button ends the request and the bot's current task.

Owners listed in Preferences can give orders in the game: whisper to the bot, mention its name, or
start the message with `!` (`!come here`). The bot answers them in the chat. Other players' messages
are never treated as instructions. On offline-mode servers names can be faked, so only use owner
orders where players have to log in.

Each provider keeps its own profile (model, API address, API key), so switching between, say,
Ollama and Gemini is one click in the model menu next to Connect, and works in the middle of a
conversation. The profiles and the login password are stored encrypted in the desktop keyring
(Secret Service: KWallet, GNOME Keyring, KeePassXC...); `~/.config/minellm/settings.ini` only
remembers which provider is in use. Without a keyring they last until the app closes.
`MC_LOGIN_PASSWORD` in the environment is used when the keyring has no password.

## How it fits together

```
 MineLLM window ──► agent loop ──► model provider (HTTP, tool calling)
        │               │
        │               └──► minemcp core (child process, Unix socket, JSON lines) ──► Minecraft server
        └─ status, game chat, log ◄── core events
```

- `src/agent.c`: the conversation and the tool loop (step limit, stop, history trimming).
- `src/llm.c`: providers; the conversation is kept in the OpenAI chat format and converted for
  Anthropic and Ollama.
- `src/core_client.c`: starts the `minemcp` daemon and speaks its control protocol; the daemon exits
  with the app.
- `data/tools.json`: the tool definitions, generated from MineMCP's MCP server by
  `tools/gen_tools.py` (needs `npm ci` in `core/mcp`). Each tool name is the core command it runs.

Developer tools (not installed): `agent_probe` runs one request without the window,
`llm_roundtrip` (with `tests/mock_gemini.py`) checks that Gemini thought signatures survive a tool call and
a provider switch, `profile_test` checks the keyring profiles (it uses the `groq` and `custom` entries), and
`window_shot` saves a PNG of the window on a headless GTK backend:

```sh
cmake --build build --target agent_probe window_shot
MC_LOGIN_PASSWORD=... ./build/agent_probe 127.0.0.1 25565 MyBot ollama qwen3:8b "look around"
MINELLM_DEBUG=1 ...   # prints the raw model replies
```

## License

MIT
