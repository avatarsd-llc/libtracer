<!--
SPDX-License-Identifier: Apache-2.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# Cloudflare — serve the docs at `docs.avatarsd.com/libtracer`

The docs are published by GitHub Pages at `avatarsd-llc.github.io/libtracer` (the
`/libtracer` prefix is GitHub's project-pages path). To serve them at
**`docs.avatarsd.com/libtracer`** — same path, your domain, and a spot for other
projects under `docs.avatarsd.com/<project>` later — Cloudflare **reverse-proxies**
the path onto the unchanged Pages origin. Nothing about the built site changes, so
every internal link keeps working; only [`docs/conf.py`](../../docs/conf.py)'s
`html_baseurl` names the public URL for canonical/OpenGraph tags.

This is a maintainer setup task on **your** Cloudflare account — Claude can't
create accounts, change DNS, or deploy Workers for you. The Worker code is
[`docs-proxy-worker.js`](docs-proxy-worker.js).

## Steps

1. **Zone.** Add `avatarsd.com` to Cloudflare (if not already) and point your
   registrar's nameservers at the two Cloudflare NS records. Wait for **Active**.
2. **DNS.** Add a **proxied** (orange-cloud) record for the `doc` hostname so
   Cloudflare receives its traffic:
   `CNAME  doc  →  avatarsd-llc.github.io`  (proxied).
   (The Worker below intercepts `/libtracer/*`; the CNAME target only matters for
   other paths.)
3. **Worker.** Dashboard → **Workers & Pages** → **Create Worker** → paste
   [`docs-proxy-worker.js`](docs-proxy-worker.js) → **Deploy**. (Or `wrangler deploy`.)
4. **Route.** On the Worker → **Settings → Domains & Routes → Add route**:
   `docs.avatarsd.com/libtracer*`  (zone `avatarsd.com`). This binds the Worker to
   that path.
5. **Verify.** Open `https://docs.avatarsd.com/libtracer/` — it should render the
   docs. Internal navigation stays under `docs.avatarsd.com/libtracer/…`.
6. **Web Analytics.** Dashboard → **Web Analytics** → add `docs.avatarsd.com`.
   Because the host is proxied, visits are counted at the edge — **no page-side JS
   beacon**, nothing rendered on the site, dashboard private to you.
7. **Merge the docs PR** ([#312](https://github.com/avatarsd-llc/libtracer/pulls))
   whenever — its only effect is the canonical `html_baseurl`; it changes no
   routing, so it's safe before or after the proxy is live.

## Notes

- **No GitHub custom domain / CNAME file** is set: the origin stays
  `avatarsd-llc.github.io/libtracer`, so the github.io URL keeps working too (both
  resolve; `html_baseurl` marks `docs.avatarsd.com/libtracer` as canonical).
- **No-code alternative:** a Cloudflare **Origin Rule** on `docs.avatarsd.com/libtracer*`
  overriding the *Host header* + *origin* to `avatarsd-llc.github.io` achieves the
  same proxy without a Worker, if you prefer rules over code.
- Want a different path (e.g. bare `docs.avatarsd.com`)? Change `PREFIX` in the
  Worker and the route, and `html_baseurl` in `conf.py`.
