## Security Rules

- **NEVER read, open, inspect, print, or parse `.env` files or files containing environment secrets.**
- Do not use commands such as `cat`, `type`, `Get-Content`, `grep`, or `Select-String` to inspect `.env` files.
- Do not read `.env`, `.env.*`, or equivalent environment files in any directory.
- Do not display, copy, log, or expose API keys, access tokens, passwords, database credentials, or other secrets.
- If environment variable names are needed, ask the user to provide the variable names without their values.
- If a task requires environment configuration, use `.env.example` with placeholder values instead.
- If a task appears to require reading secrets, stop and ask the user for a safe alternative.