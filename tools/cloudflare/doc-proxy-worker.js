// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
//
// Cloudflare Worker: transparently reverse-proxy
//   https://doc.avatarsd.com/libtracer/*   ->   https://avatarsd-llc.github.io/libtracer/*
//
// GitHub Pages serves this project at the /libtracer/ path prefix and routes by
// Host header, so a plain custom domain can't preserve the sub-path. This Worker
// keeps the public URL (doc.avatarsd.com/libtracer/...) while fetching from the
// unchanged Pages origin — so every internal link, which already uses the same
// /libtracer/ prefix, resolves without any change to the built site. It also lets
// doc.avatarsd.com host other projects under their own sub-paths later.
//
// Deploy: see tools/cloudflare/README.md. Route it at `doc.avatarsd.com/libtracer*`.
const ORIGIN = "https://avatarsd-llc.github.io";
const PREFIX = "/libtracer";

export default {
  async fetch(request) {
    const url = new URL(request.url);

    // Only this Worker's sub-path is ours; anything else on the host is not.
    if (url.pathname !== PREFIX && !url.pathname.startsWith(PREFIX + "/")) {
      return new Response("Not found", { status: 404 });
    }
    // Bare /libtracer -> /libtracer/ so relative asset URLs resolve.
    if (url.pathname === PREFIX) {
      return Response.redirect(url.origin + PREFIX + "/", 301);
    }

    const upstream = ORIGIN + url.pathname + url.search;
    const headers = new Headers(request.headers);
    headers.delete("host"); // let fetch set Host from the upstream URL (Pages routes by Host)

    const proxied = new Request(upstream, {
      method: request.method,
      headers,
      body: request.method === "GET" || request.method === "HEAD" ? undefined : request.body,
      redirect: "manual",
    });

    const resp = await fetch(proxied);

    // Rewrite any origin-absolute redirect Location back onto the public host so
    // GitHub's own trailing-slash/directory redirects don't leak the github.io origin.
    const out = new Headers(resp.headers);
    const loc = out.get("location");
    if (loc) out.set("location", loc.replace(ORIGIN, url.origin));

    return new Response(resp.body, { status: resp.status, statusText: resp.statusText, headers: out });
  },
};
