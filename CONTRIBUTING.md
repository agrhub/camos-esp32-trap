# Contributing

## Language

- Code, comments, docs, commit messages, log and serial-console messages are in **English**.
- Vietnamese only in the UI string tables (the `vi` tables of `firmware/main/portal.html`, `firmware/main/console.html`,
  `docs/flash/index.html`), in `README.vi.md`, and in data (sample values, operator or place names).
- The pages default to English; Vietnamese is the second UI language.

## Rules

- Never change the meaning of an existing protocol field ([docs/dev/protocol-v1.md](docs/dev/protocol-v1.md));
  additions go in [docs/dev/PROTOCOL_ADDITIONS.md](docs/dev/PROTOCOL_ADDITIONS.md).
- No secrets in the image or in git: certificates, keys and device tokens are per device (self-registration or
  `tools/factory.*`). `firmware/defaults.local.json` and `tools/out/` stay git-ignored.
- No real serial numbers, verify codes, Wi-Fi names or IP addresses in examples or screenshots: use
  `ESP000000000001` / `ABC123` and the mock camera (`tools/mock/server.js`).
- Measure before writing a number into the docs, with the date and the setup; otherwise write "not measured".
- Build directories are `firmware/build-<board>` (git-ignored). Host tests: `make -C tests`.
- Commits: conventional prefixes (`feat:`, `fix:`, `docs:`, `chore:` ...), one logical change each.
