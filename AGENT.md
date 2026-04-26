# AGENT.md

## Scope
- This repository tracks `USTC-CG/USTC_CG_26` on `main`.
- Homework branches `hw1` to `hw6` should inherit this file from `main` when practical.

## Working Style
- Read the relevant files before changing code.
- Make the smallest change that fully solves the task.
- Do not refactor unrelated code or add speculative abstractions.
- Prefer editing existing files over creating new ones.

## Verification
- Run the narrowest relevant build, test, or check for the code you changed.
- On Windows, this build must be run only in a real foreground PowerShell window.
- Before running the specified 3D build command sequence, an agent must open a completely fresh foreground PowerShell window. Do not reuse an existing terminal session.
- Successful manual command sequence in `build`:
  - `$vsPath = & (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe") -property installationPath`
  - `Import-Module (Join-Path $vsPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll")`
  - `Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments "-arch=x64"`
  - `cmake .. -G Ninja`
  - `ninja`
- Successful script launch command in `build`:
  - `powershell -NoLogo -ExecutionPolicy Bypass -File ..\scripts\build_windows_ninja.ps1`
- `scripts/build_windows_ninja.ps1` is the approved wrapper for this workflow.
- This build command sequence is fixed. Do not modify, simplify, reorder, replace, parameterize, or partially run it.
- Always run the full build. Do not change it to incremental, target-specific, background, hidden, tool-hosted, or otherwise reduced execution.
- The script must keep the six build commands intact and only add logging plus automatic terminal exit.
- The script already normalizes conflicting `Path` / `PATH` process environment entries before `Enter-VsDevShell`. Do not add a different environment workaround unless this one is proven insufficient.
- The script must overwrite `build/build_windows_ninja.log` on each run.
- The log must include terminal-visible output from both `cmake` and `ninja`, including `ninja` failure output.
- If an agent needs to verify this build, it must strictly use the successful script launch command above.
- A normal successful run usually completes within about 30 seconds.
- When a tool-hosted agent launches the foreground build window, it must not rely on one long blocking wait command to infer completion. Launch first, then inspect process and log state with separate follow-up commands.
- If the foreground PowerShell window has not exited automatically after 2 minutes, treat that as abnormal and inspect status immediately.
- Status inspection commands from the repository root:
  - `Get-Process | Where-Object { $_.ProcessName -in @('powershell','cmake','ninja') } | Select-Object ProcessName,Id,StartTime,CPU`
  - `Get-Content D:\vsprojects\USTC_CG_26\Framework3D\Ruzino\build\build_windows_ninja.log -Tail 120`
- If you could not verify something, say so explicitly.
- Report failures faithfully. Do not claim success without checking.

## Safety
- Ask before destructive or hard-to-reverse actions.
- Do not overwrite or revert user work unless explicitly asked.
- Treat unexpected local changes as intentional until proven otherwise.

## Communication
- Keep updates concise and high-signal.
- Surface assumptions, risks, and blockers clearly.

## Local Records
- Maintain a document for each homework that records the changes made during the work.
- Export the corresponding chat history as JSON for each homework when requested as part of the workflow.
- When taking over a task, read the previous homework logs and chat exports first to understand the existing context before making changes.
- Store these logs and chat exports only in local, homework-classified directories.
- Do not commit, push, or otherwise upload those local logs or chat export files to any remote or cloud service unless explicitly requested.

## Homework Reports
- When a task includes preparing or revising a homework report, treat the report as part of the deliverable rather than an afterthought.
- Follow a clear report structure. Unless the homework explicitly says otherwise, include at least:
  - objective / completed items
  - algorithm principle
  - implementation details
  - experiment setup
  - experiment results
  - analysis and conclusion
- Explain the core algorithm with formulas, not only prose. Use proper math syntax instead of screenshots of formulas when editing report sources.
- Every figure and table should have a title or caption and nearby explanatory text that tells the reader what to look at and why it matters.
- If AI tools were used, explicitly state:
  - which tools were used
  - what they were used for
  - what parts were accepted or rejected
  - how the AI-assisted output was verified or analyzed
- At minimum, ensure the report contains:
  - algorithm-principle explanation
  - experiment-result presentation
  - experiment-result analysis and conclusion
- Bonus-oriented improvements worth adding when evidence exists:
  - deeper principle analysis with personal understanding
  - more datasets
  - module / parameter ablations
  - comparison between multiple methods
  - cleaner layout and presentation
- Preferred final submission format is PDF, but keep an editable source report in the homework directory. If the user explicitly says not to export PDF in the current task, stop at updating the source materials.
