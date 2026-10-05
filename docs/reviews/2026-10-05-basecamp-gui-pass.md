# Basecamp 0.3.1 GUI pass, 2026-10-05

Real Basecamp 0.3.1 under Xvfb, separate profile, local test repo. The other user is headless
node A (logosctl) on the live logos.test fleet. Every flow was clicked through; node A checked the
result over the wire.

| flow | result |
|---|---|
| profile (Me) | OK. The name reached A at once and survives a restart |
| publish with the real file dialog | OK. Thumbnail and fingerprint made; a 4.7 MB STL publishes in ~0.2 s |
| publish by pasted path | OK |
| new version | OK (0.1.4). Files carry over by hash, with "remove"; a remix's new version keeps its parent |
| remix, with a photo | OK. Parent linked by title + creator; the photo shows in the version gallery |
| post a make with a photo | OK. A received the photo within ~30 s |
| like / unlike, comment | OK (earlier pass) |
| download from another user | OK (earlier pass). Byte-identical in 50 s |
| retract, with a reason | OK (0.1.4). A hides the model immediately and shows the reason; it stays in your own My models, marked "retracted" |
| restart | OK. Profile, catalogue, my models and downloads survive |
| browse: 15 models, scrolling | OK |

## Fixed in this pass (0.1.3, 0.1.4)

- **Retract** was missing from the view (the core had it). Added a themed confirm dialog with a reason.
- **Remix parent** showed as a raw id ("Remix of d3af9289 v1"). It's now a link with the remixed
  version's title and creator (the core's `getModel` adds them).
- **Version photos** were never shown. Added a gallery under the main picture.
- **The button row moved** when "Open folder" appeared, and a click meant for Remix toggled Like
  (it happened in this test). The buttons now sit in fixed slots: Open folder is disabled, not hidden.
- **New version dropped every file.** Files now carry over by hash (the draft accepts
  `{sha256, name}` for a blob the node holds).
- **Remix context:** the form now says what you're remixing, and warns when the original's licence
  is ND (no derivatives), SA (same licence required) or NC (stay non-commercial).
- The file dialog opens in your home folder (photos in Pictures), with a 3D-models filter.
- The toast floats, so it no longer pushes the page down under the cursor.
- "1 likes" → "1 like". The node counters are labelled "since start".
- Your own retracted models stay in My models.

## Seen, not fixed

- An exact copy (the same STL republished by another user) gets no label. That's roadmap item 3.
- The Qt file dialog shows a wrong size for some files (2.00 MiB for a 4.7 MB STL). Qt's bug, not ours.
- New versions don't carry over photos (the thumbnail is regenerated).
- The licence warning was checked in code but not clicked in the GUI with a real ND original.
