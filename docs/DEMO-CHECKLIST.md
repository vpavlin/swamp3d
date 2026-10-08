# Showing Swamp to people: checklist (2026-10-08)

Not endorsed by or affiliated with Logos; built on the Logos tech stack.

`[you]` = only vpavlin can do it (GitHub settings, a decision). Everything else Claude can do on atlas.

## 0. Decide first

- [ ] `[you]` **How will people try it?**
  - (a) **Watch only:** you demo from your laptop. Needs only section 2.
  - (b) **Install it themselves**, in the room or remotely. Needs all of sections 1-4.
- [ ] `[you]` **Where the hub runs.** Suggestion: the existing VPS, next to `scala-hub.service`.
- [ ] `[you]` **Repo name:** `vpavlin/swamp`. Public from today (unlocked 2026-10-06).

## 1. Must do before anyone else installs it

### Hub

Without a hub, people on home networks can't fetch each other's files, and a late joiner finds an
empty catalogue.
- [ ] Run a `logosctl` session with `SWAMP_HUB=1`, using the 0.5.2 core and the **public** dependency
      versions.
  - A hub is automatically an indexer and follows every category.
  - The hub's `~/.logos_storage/config.json` needs: `nat=extip:<public IP>`, AutoNAT server, relay
    server, a long `block-ttl`, and a fixed `listen-port` open in the firewall.
  - Run it as a `systemd` unit with `Restart=always`.
- [ ] Seed it with 5-10 nice demo models in several categories, published **from the hub**. They're
      then fetchable from a public IP whatever happens to attendees' NATs. Use real STLs with
      pictures, licence CC-BY.

### Public Basecamp repo

- [ ] Publish `swamp_core` and `swamp` 0.5.2 to both apps.vpavlin.xyz surfaces, additively, with
      `publish-public-basecamp.py`.
  - The dependencies are already public: loam_core 0.6.1, delivery_module 0.3.0, keycard 1.1.0,
    ble_mesh 0.2.1.
  - Pushing needs write access to `vpavlin/logos-apps` and `vpavlin/logos-basecamp-modules`. `[you]`
    if atlas has no key for them.
- [ ] **Version trap.** The *documented* repo URL (`apps.vpavlin.xyz/logos-repo.json`) still lists
      **delivery_module 0.9.0**, the old fork. It outranks 0.3.0, so a Basecamp 0.3 user would get the
      fork, and Swamp (like every ported app) would never connect.
  - Either remove 0.9.0 from the release catalogue (it breaks 0.2.x users of that URL), or
  - tell people to add **`https://apps.vpavlin.xyz/basecamp/logos-repo.json`**, which has only 0.3.0.
  - `[you]` to decide which.

### Tests

- [ ] **Re-test against the public dependencies.** 0.5.2 was tested with loam_core 0.5.4 and
      ble_mesh 0.2.0; the public versions are 0.6.1 and 0.2.1. Run `hub/two-node.sh` with the public
      .lgx files.
- [ ] **Fresh install, the way an attendee does it.** Basecamp 0.3.1 with a new `HOME`. Add the
      public repo URL and install Swamp (dependencies should come along). Check:
  - status reaches Connected within about 30 s;
  - the hub's demo models appear;
  - Download works and the copy is byte-identical;
  - global search finds a model outside the categories you follow.
- [ ] **Cross-network test.** A desktop on a different network from the hub (laptop on a phone
      hotspot):
  - its upload must be fetchable by a third node, through the hub's cache;
  - it must fetch a hub-seeded model with no Storage config edits. If it needs the hub in its
    `bootstrap-node` list, that becomes an attendee instruction.
- [ ] **Two GUI users at once.** A likes and comments on B's model; B sees it within seconds.
- [ ] **Restart.** Quit and reopen Basecamp: identity, models and downloads are kept.
- [ ] Re-check the dependency install pulls nothing old. The release catalogue has storage_module
      2.1.3, and swamp_core depends on storage_module; the bundled 3.0 has to win.

## 2. Demo script (about 10 minutes)

1. The idea in one sentence: a model library no company can take down.
2. **Browse:** categories, a model page with pictures, versions, remix, makes.
3. **Publish live:** drop in an STL. Show the thumbnail and fingerprint appearing right away, then
   the upload finishing in the background.
4. **Second machine:** it appears there. Like or comment and watch it arrive.
5. **Global search:** find something in a category you don't follow, open it, download it (verified).
6. **Open in slicer** (OrcaSlicer).
7. **Optional:** print on the A1 over the LAN. Only after a real-A1 dry run (section 4); otherwise
   skip it.
8. **What's next:** theft labels (shape fingerprints), anchoring on LEZ, Android.

Have a fallback: screenshots in `docs/screenshots/` and the website, in case the network misbehaves.

## 3. Publish the code and the site

- [ ] `[you]` Create the GitHub repo `vpavlin/swamp` and add atlas's deploy key with write access.
      Deploy keys are one per repo: generate `~/.ssh/atlas_deploy_swamp_ed25519` and the
      `github-deploy-swamp` alias.
- [ ] Before pushing:
  - check `git ls-files` for secrets, data dumps, absolute paths and logs (`00000.log` sits in the
    repo root);
  - `bench/data` stays ignored;
  - add a LICENSE `[you]`: which one? MIT/Apache-2.0 fits; AGPL slicers are only called through
    their CLI.
- [ ] Push `main`.
- [ ] Website: add an **Install** section (the repo URL, the steps, "Not endorsed by or affiliated
      with Logos"), plus links to GitHub and the kit.
  - Publish it on GitHub Pages: `[you]` turn on Pages, from `main` `/site` or a `gh-pages` branch.
    Pick a domain, e.g. `swamp.vpavlin.xyz`, `[you]` DNS.
- [ ] Add a Swamp card to the storefront (needs `display_name` + `icon.png`; the view has both), and
      run "Build storefront".
- [ ] Put the README link to the website and the install URL at the top.

## 4. Known limitations: say them out loud

- **Test network.** logos.test resets; RLN is off by config.
- **The hub is ours:** bootstrap centralisation, said openly. Anyone can run another.
- **Files are best-effort** (Storage has no durability guarantee); the hub keeps copies.
- **LAN printing has never run on a real printer.**
  - `[you]` A real A1 dry run before showing it: LAN-only + Developer Mode, then follow the 0.5.2
    TEST.md.
  - Turn cloud printing back on afterwards.
- **Not built yet:** theft labels, LEZ anchoring, live query hubs, Android.
- **No security audit.** Identities are keys on the device; there's no recovery.
