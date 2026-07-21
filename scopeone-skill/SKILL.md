---
name: scopeone
description: Configure and operate a running ScopeOne microscopy application through its bundled stdio MCP server. Use when an MCP-capable AI agent needs to connect to ScopeOne, inspect microscope state, load Micro-Manager configurations, control preview, exposure, ROI, or stages, manage processing, recording, or mosaics, analyze frames, or work with image layers and annotations. Do not use for ordinary ScopeOne source editing, builds, packaging, or code review unless the task also requires controlling the running application.
---

# ScopeOne

Control the running desktop application through `ScopeOneMcpServer`. Treat the
Local API as the authority and use MCP tools instead of clicking GUI controls or
calling the Python client.

## Install the shared Skill

Use this same folder for every compatible agent instead of maintaining separate
Codex and Claude variants:

- Codex: copy or link it to `$CODEX_HOME/skills/scopeone`, or to
  `~/.codex/skills/scopeone` when `CODEX_HOME` is unset.
- Claude Code: install or load this folder as a plugin. When prompted, select
  the `ScopeOneMcpServer` executable installed with ScopeOne.

## Select the path

1. If the request only concerns source code, building, packaging, or review,
   follow the repository instructions and do not configure MCP.
2. If ScopeOne MCP tools are already available, skip registration.
3. If the tools are unavailable and the user wants runtime control, repair the
   active client's MCP configuration before attempting microscope operations.

## Configure the MCP client

1. Locate `ScopeOneMcpServer.exe` beside an installed `ScopeOne.exe`, in the
   portable package, or under `build/Release` in a source checkout. On Linux,
   use the corresponding executable without the `.exe` suffix.
2. Do not launch the MCP server manually. The MCP client starts it as a stdio
   child process after registration.
3. In Claude Code, use the plugin's required `ScopeOne MCP Server` setting. The
   plugin starts that executable automatically. Do not also run `claude mcp add`.
   If the configured path is missing or wrong, ask the user to update the plugin
   setting and reload plugins.
4. In Codex, explain that registration changes the user's configuration and
   obtain explicit approval before making that change. Check the existing entry:

   ```text
   codex mcp get scopeone
   ```

5. If the Codex entry is absent, replace `ABSOLUTE_SERVER_PATH` with the
   resolved executable path and run:

   ```text
   codex mcp add scopeone -- "ABSOLUTE_SERVER_PATH"
   ```

6. If an existing Codex entry points to the wrong executable, explain the
   mismatch and obtain explicit approval before running `codex mcp remove
   scopeone`, then add the corrected entry.
7. Ask the user to restart the agent or start a new session after registration.
   Do not claim the MCP tools are available in the current session until they
   actually appear.

## Establish state

1. Ensure the ScopeOne desktop application is running. Launch it only when the
   user explicitly requests that action.
2. Call `capabilities` to discover the current operation catalog.
3. Call `state_snapshot` before changing hardware or acquisition state.
4. Use the current MCP schemas as the parameter authority. Do not reconstruct
   requests from memory when a schema is available.

## Execute operations

- Prefer one semantic ScopeOne operation over GUI automation or a sequence of
  low-level property edits.
- Read back state after each mutation. Use hardware reads for exposure, ROI,
  properties, and stage positions; use status calls for acquisitions.
- Poll `experiment_status` or `stage_mosaic_status` for long-running work and
  stop only at a terminal state or when the user requests cancellation.
- Use `layer_frame` for raw, processed, static, mosaic, or Gallery image data.
- Remember that all clients share one frame mapping. Finish reading one exported
  frame before requesting another.
- Read [references/tool-routing.md](references/tool-routing.md) for canonical
  operation sequences and troubleshooting.

## Apply safety boundaries

- Respect every MCP confirmation field. Never set `confirm=true` without the
  user's explicit approval for that operation.
- Treat configuration changes, camera settings, ROI changes, stage motion,
  acquisition, cancellation, file writes, and destructive removals as real
  effects on laboratory state.
- Never infer a stage destination, recording directory, output name, or
  destructive target when the user has not supplied enough context.
- Do not expose UI-only actions such as opening dialogs, changing docks, or
  simulating button clicks. Use domain operations only.
- Do not fall back to Python merely because MCP setup is incomplete. Use Python
  only when the user explicitly requests a script, notebook, or Python workflow.

## Recover cleanly

- If the client cannot start the MCP server, verify the registered absolute path
  and confirm that the executable belongs to the same ScopeOne installation.
- If the MCP server starts but reports that the Local API is unavailable, ask
  the user to start ScopeOne and ensure only the intended instance is running.
- If the Claude plugin is enabled but tools are absent, verify its configured
  executable and reload plugins. For Codex, restart it and inspect
  `codex mcp get scopeone`.
- If an operation fails, report its exact error, refresh state, and correct the
  request. Do not retry physical or destructive operations blindly.

For raw protocol or Python-client work only, consult
`ScopeOneCore/python/scopeone/README.md` in the repository.
