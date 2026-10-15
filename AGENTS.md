# AGENTS.md

## Code formatting
- Indent with 4 spaces.
- Use Allman brace style: opening braces go on their own line.
- Separate logical chunks of code with a blank line: a block of variable definitions from the code after it, a blank line before a `return` at the end of a function, between function definitions, and between conditional statement blocks.

## Git
Do not run git commands that change local files or git history (for example add, commit, checkout, switch, merge, rebase, reset, stash, branch, worktree, or push). Read-only commands such as git status, git diff, and git log are fine. Only run a state-changing git command when the user explicitly asks for it in their message.
