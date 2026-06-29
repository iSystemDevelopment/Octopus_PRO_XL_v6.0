# Local workspace (not for GitHub)

Machine-local scratch, dev tooling, and operator notes. The whole `local/` tree is **gitignored**.

**Stays at repo root (also gitignored, not moved):**

- `.claude/` — Cursor / agent settings
- `seo/` — robots + sitemap files uploaded manually to VPS

## Layout

| Path | Contents |
|------|----------|
| `dev/` | `node_modules/`, `package.json`, ESLint config (Puppeteer scratch tooling) |
| `scratch/` | `temp*.js`, `test.js`, `git_log.txt` — experiments and logs |
| `operator/` | `todo.md`, `v6.3.00.md`, `project_summary.md` — planning / deploy notes |
| `assets/` | Extra images not referenced by the shipped site (e.g. source PNG exports) |

Shipped product assets at repo root: `octopus-midi-mode.png`, `octopus-app-hero.jpg`, `logo.jpg`, etc.
