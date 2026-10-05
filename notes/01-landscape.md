# 01 — Landscape: centralized 3D-model / maker-project sharing sites

Compiled 2026-10-05 via web search. Every claim carries a source. **[?]** means unverified, old, or only supported by community/forum sources. Fee numbers change often, so check them again before relying on them in a design.

---

## 1. Per-platform facts

### Printables (Prusa Research, CZ)
- **Business model:** a funnel for Prusa hardware and filament. Free downloads, plus a paid Store and Printables Clubs (subscriptions).
- **Store:** Prusa takes a **20% fee**. Stripe and currency-conversion fees are charged on top. Minimum price is $5. Sellers start with 1 Store slot and unlock more by sales count: 5 slots after 5 sales, 10 after 25, unlimited after 50 ([3DPI, 2023](https://3dprintingindustry.com/news/prusa-research-launches-paid-3d-printable-model-marketplace-227446/); [All3DP](https://all3dp.com/4/prusa-research-launches-printables-store-paid-model-repository/)). These are the 2023 launch terms. I did not find a 2025 revision **[?]**.
- **Clubs:** up to 5 tiers, each $3–$100 a month. A creator needs **level 18** to open a Club, or can submit a portfolio instead. Supporters earn loyalty badges. Prusa's cut is about 10%, paid out via PayPal ([Prusa blog](https://blog.prusa3d.com/printables-clubs-are-live-you-can-now-support-the-creators-you-love-on-printables_81063/); [All3DP](https://all3dp.com/4/printables-to-launch-patreon-like-subscription-service/)).
- **Rewards:** "Prusameters" are earned from downloads and makes and are redeemable for Prusa vouchers and goods. The parallel Prusa Reward Program (referrals, $30 voucher) was restructured for orders from 2026-02-25 ([Prusa help](https://help.prusa3d.com/ja/article/prusa-reward-program_670212)). I did not find current Prusameter rates **[?]**.
- **Page contents:** files (STL, 3MF, STEP and others), photos, description, "makes" (user print photos), comments, and remix origin ("The author remixed this model", plus a recommended section "Differences of the remix compared to the original") ([printables.com example](https://www.printables.com/model/500004-baren-remixx)). It also has collections and contests ([contests](https://www.printables.com/contest/498-winter-holidays-decorations-2025)).
- **Licenses:** a list of CC variants plus a non-commercial "Standard Digital File License" adapted from MyMiniFactory's ([forum analysis](https://forum.bambulab.com/t/understanding-makerworlds-standard-digital-file-license/257609)).
- **AI:** AI-generated models must be tagged ([3druck](https://3druck.com/en/programs/bambu-lab-takes-stronger-action-against-ai-slop-on-3d-model-platform-makerworld-57153725/)).
- **Slicer integration:** PrusaSlicer 2.9+ has a built-in Printables tab. It can search, download STL/3MF straight into the slicer, and send G-code to a printer via Prusa Connect, but only when the G-code was sliced for that printer ([Prusa help](https://help.prusa3d.com/article/printables-in-prusaslicer_822448)).
- **File limits:** above MakerWorld's, with no combined cap observed (forum, 2024–25). I found no official number **[?]**.

### MakerWorld (Bambu Lab, CN)
- **Business model:** a funnel for Bambu printers and filament, with heavy incentives for creators.
- **Points:** creators earn points from downloads, prints, ratings and **Boosts**. Since June 2025 the earning formula is algorithmic and hidden. A Boost is worth 12 points (15 on Exclusive models). **Regular points cannot be cashed out.** They buy store goods: about 490 points ≈ a $40 gift card, community-reported. **Exclusive points cash out at $0.066 per point, with a $100 minimum** ([bambuhub guide, updated 2026-10-03](https://bambuhub.net/guides/makerworld-points-rewards)). The March 2025 update allows up to 3 Boost tokens a week and is aimed at "engineering effort" ([Bambu blog](https://blog.bambulab.com/boosting-what-matters-rewarding-engineering-and-design-effort)).
- **Exclusive Model Program:** eligibility needs 100 total prints. Models are locked in for **at least 90 days**, and publishing one elsewhere revokes its points ([policy](https://makerworld.com/exclusive-model-policy)). Creators complain about losing *all* points when a model is removed from the program, and about vague rules ([forum](https://forum.bambulab.com/t/my-model-was-removed-from-exclusive-model-program-lost-all-points/127138); [forum](https://forum.bambulab.com/t/all-models-removed-from-exclusive-program-points-revoked/199261)).
- **Paid options:**
  - Commercial License Membership: $3–300 a month, Bambu takes **10%**, paid via PayPal. Creators are vetted (200 followers and 600 prints) ([All3DP](https://all3dp.com/4/bambu-lab-launches-makerworld-commercial-license-membership/)).
  - **Crowdfunding** (Aug 2025): all-or-nothing, goals of $500–$50k over 7–60 days. Fees are **10% MakerWorld plus about 8% Stripe** ([3Dnatives](https://www.3dnatives.com/en/bambu-lab-launches-new-crowdfunding-feature-on-makerworld-06082025/); [forum](https://forum.bambulab.com/t/introducing-makerworld-crowdfunding-feature/186216?page=2)).
  - No plain pay-per-download as of the guide's 2026-10 update ([bambuhub](https://bambuhub.net/guides/makerworld-points-rewards)).
- **Page contents:** raw model files plus **"print profiles"**. A print profile is a sliced Bambu Studio/OrcaSlicer 3MF that enables one-click printing to a Bambu printer through Bambu Cloud. A model can have several profiles from different users ([Bambu wiki](https://wiki.bambulab.com/es/makerworld/tutorials/how-to-upload-models)). Updates replace the file in place, with an optional change log. Users ask for update notifications, which suggests there is no real version history ([forum](https://forum.bambulab.com/t/feature-request-notify-users-of-profile-updates/137504)) **[?]**.
- **File limits:** 250 MB combined for raw models and 150 MB per profile (possibly raised to 200 MB). Models with more than ~1.2M vertices are rejected ([forum](https://forum.bambulab.com/t/max-raw-model-and-print-profile-file-size-limit-too-low/64114); [forum](https://forum.bambulab.com/t/what-is-the-makerworld-print-profile-triangle-vertex-limit/184691)).
- **AI:** Bambu also runs MakerLab AI generators, including image-to-3D ([3DPI](https://3dprintingindustry.com/news/bambu-lab-launches-new-ai-3d-model-generator-233736/)). Policy requires an "AIGC" tag and at least one **real print photo** matching the file. AI images are not allowed as cover images ([AI policy](https://wiki.bambulab.com/en/makerworld/ai-policy); [Fabbaloo](https://www.fabbaloo.com/?p=234795)).
- **Theft handling:** Bambu says it helped ~2,000 creators with 4,000+ re-upload cases ([Bambu blog](https://blog.bambulab.com/exclusive-model-program-cash-rewards-and-copyright-support)). Creators still report stolen paid models that stay up after dozens of reports ([forum](https://forum.bambulab.com/t/suspicious-reuploads-stolen-models/181314); [forum](https://forum.bambulab.com/t/user-reuploading-exclusive-models-with-free-to-sell-in-title/142888)).
- **Payout regions:** bank transfer for Exclusive cash. Some countries only get SWIFT with a $25 fee (Bosnia example), and USD-only payouts hurt Canadians ([forum](https://forum.bambulab.com/t/is-redeem-cash-option-available-worldwide/189658)).

### Thingiverse (founded 2008 by MakerBot; owned by UltiMaker, then **MyMiniFactory since 2026-02-12**)
- **Business model:** ad-supported and free. MyMiniFactory bought 100% of it to add "sustainable monetization" while keeping existing free models free ([UltiMaker](https://ultimaker.com/learn/myminifactory-acquires-thingiverse/); [3Dnatives](https://www.3dnatives.com/en/myminifactory-thingiverse-12022026); [All3DP](https://all3dp.com/4/the-end-of-an-era-myminifactory-acquires-thingiverse/)).
- **Size:** 2.5M to 6M+ designs depending on the source, and ~8M users combined.
- **Features:** CC licenses, remixes, makes, collections, and the OpenSCAD Customizer.
- **AI:** an AI label and filter were added in early 2025 ([All3DP](https://all3dp.com/4/thingiverse-adds-ai-labels-lets-users-filter-machine-created-works/)). The new owner applies a "SoulCrafted" no-AI policy and is removing existing AI content ([TechRadar](https://www.techradar.com/pro/3d-printing-saved-from-ai-as-myminifactory-acquires-thingiverse-over-8-million-users-and-an-archive-of-over-2-5-million-things-set-to-be-protected?rand=141)).
- **Weapons:** in July 2025, after pressure from Manhattan DA Bragg, it purged functional firearm files. Detection is **AI-flagged with human sign-off**. Props and airsoft remain allowed ([The Register, 2025-07-23](https://www.theregister.com/2025/07/23/thingiverse_drops_3d_gun_designs)).
- **Reliability:** a long record of broken search, slow uploads and weak moderation ([Fabbaloo](https://www.fabbaloo.com/?p=234955)). Outages were reported in Dec 2024 and on 2025-01-09 ([outage tracker](https://digistatement.com/thingiverse-website-down-not-working-users-are-getting-a-502-bad-gateway-error)) **[?]** (secondary trackers only).

### Cults3D (Cults SAS, Brive-la-Gaillarde, FR, founded 2014)
- **Size:** about 12M members, ~200k designers and 2.3M models as of May 2025. It also hosts laser, CNC, papercraft and PCB files ([Wikipedia](https://en.wikipedia.org/wiki/Cults_(3D_printing_marketplace))).
- **Fees:** free and paid models. Cults takes **20% commission**, about 5% of which is bank costs. Designers get 80% via PayPal and pay no subscription ([Cults legal](https://cults3d.com/de/legal)).
- **Takedowns:** under EU law, not the DMCA. Complainants must contact the alleged infringer before filing, which critics say lets thieves profit and then delete ([forum, 2026-07](https://forum.bambulab.com/t/cults-appears-to-be-violating-the-dmca-but-im-no-big-city-lawyer/256145)). Games Workshop requested removal of 200+ models ([Fauxhammer](https://www.fauxhammer.com/?p=21598)).

### MyMiniFactory (London; now also owns Thingiverse)
- **Focus:** tabletop miniatures, with a curated, human-made ("SoulCrafted") emphasis.
- **Store fees:** **15% / 12.5% / 10%** commission by creator tier, plus processing of 2.9% + $0.30, or 5% + $0.10 on micropayments, plus 1% for non-US buyers ([MMF fees](https://creator.myminifactory.com/store-manager-fees)).
- **Tribes:** subscriptions where creators get 92% before processing ([3DPI](https://3dprintingindustry.com/news/myminifactory-launches-its-new-tribes-subscription-service-for-3d-designers-196769/)).
- **Totals:** says it has paid creators $100M+ ([pulse2](https://pulse2.com/myminifactory-acquires-thingiverse-to-create-largest-creator-first-community)).
- **Licenses:** the original source of the "Standard Digital File License", which is personal, non-commercial, no-redistribution ([forum](https://forum.bambulab.com/t/understanding-makerworlds-standard-digital-file-license/257609)).
- **Takedowns:** has received Games Workshop notices ([Fauxhammer](https://www.fauxhammer.com/?p=21598)).

### Thangs (Physna Inc., US)
- **Differentiator:** **geometric/shape search**. Deep learning indexes the mesh itself, so you can search by shape and find parts that fit ([GeoWeek](https://www.geoweeknews.com/articles/thangs-3d-model-search-engine-launches-with-more-than-a-million-searchable-objects-aims-to-be-a-3d-google/)). It also offers automated version control and team collaboration ([3DPI](https://3dprintingindustry.com/news/physna-launches-thangs-a-deep-learning-based-3d-model-search-engine-174672/)).
- **Memberships:** downloads, monthly-release and chapter-based types. The platform fee is **14%** plus processing and FX. Creators earning >$7.5k a month get 2% off, and another 2% off if exclusive to Thangs ([Thangs help](https://thangs.com/resources/help-center-articles/what-does-thangs-charge-designers); the page returned 403 to me, so this comes from the search snippet **[?]**).

### Instructables (Autodesk since 2011)
- **Content:** step-by-step project write-ups with photos per step, materials/BOM lists and attached files. It is not a file repository.
- **Monetization:** ads (~$10M a year by one estimate **[?]**), monthly sponsored contests, and a Pro membership earnable by getting featured ([Wikipedia](https://en.wikipedia.org/wiki/Instructables)).
- **Ownership risk:** Autodesk laid off ~1,350 staff in Mar 2025 and ~1,000 in Jan 2026 to refocus on AI and cloud ([CG Channel](https://www.cgchannel.com/2026/01/autodesk-lays-off-a-further-1000-staff/)). I found no Instructables-specific changes **[?]**, but its API appears to be served from a Tinkercad domain ([ibles-api.tinkercad.com](https://ibles-api.tinkercad.com/about/)), which shows how tightly it is coupled to Autodesk.

### Fee summary (platform cut, before payment processing)
| Platform | One-off sales | Subscriptions | Other |
|---|---|---|---|
| Printables | 20% | ~10% (Clubs) | Prusameters → vouchers |
| MakerWorld | n/a | 10% (Commercial Membership) | Crowdfunding 10% + ~8% Stripe; points |
| Cults3D | 20% (incl. ~5% bank) | — | — |
| MyMiniFactory | 10–15% by tier | ~8% (Tribes) | — |
| Thangs | ? | 14% (−2/−4%) | — |

---

## 2. Pain points and controversies relevant to a decentralized design

1. **Vendor lock-in through print profiles.**
   - MakerWorld's one-click profiles are Bambu Studio 3MFs using vendor extensions. Users report they won't open in PrusaSlicer or Cura. Bambu upstreamed 3MF Production Extension support, merged in PrusaSlicer 2024-03-21 ([Bambu wiki](https://wiki.bambulab.com/en/software/bambu-studio/3mf-compatibility); [forum](https://forum.bambulab.com/t/3mf-files-from-makerworld-dont-work-in-cura/77506)).
   - Slicer settings live in **vendor-specific folders inside the 3MF** ([SimplyPrint](https://simplyprint.io/articles/what-is-a-3mf-file)), so "the profile" is really per-slicer.
   - Rewards are tied to the platform too: Exclusive points need 90-day exclusivity.
2. **Firmware/cloud control.**
   - In January 2025 Bambu firmware added an "authorization" layer. Printing over LAN or cloud, motion control, AMS and firmware updates all had to go through Bambu Connect, which broke direct third-party control (OrcaSlicer, Home Assistant). Bambu answered with a LAN-only "Developer Mode" ([All3DP](https://all3dp.com/4/bambu-lab-limits-third-party-printer-control-with-new-security-update/); [All3DP](https://all3dp.com/4/bambu-lab-responds-to-security-update-controversy-promises-developer-mode/); [3Dnatives](https://www.3dnatives.com/en/bambu-lab-at-the-heart-of-a-controversy-an-update-that-divides-the-user-community-220120255/)).
   - In August 2024 a Bambu Cloud outage replayed queued jobs overnight and damaged printers ([All3DP](https://all3dp.com/4/bambu-lab-cloud-error-causes-prints-to-start-unprompted-users-printers-damaged/)).
   - Takeaway: "print now" through someone else's cloud is a liability.
3. **Ownership churn and link rot.** Thingiverse passed MakerBot → Stratasys → UltiMaker → MyMiniFactory. Free models are promised to stay free, but policies, such as AI removal, change under new owners. When creators leave or delete accounts their models disappear, and remix chains point at dead pages. I found no systematic public archive of Thingiverse **[?]**.
4. **Takedowns and IP pressure.** Games Workshop and LEGO send mass notices ([All3DP](https://all3dp.com/4/lego-targeting-maker-communitys-3d-models-with-infringement-notices/)). Prosecutors push for firearm filtering ([The Register](https://www.theregister.com/2025/07/23/thingiverse_drops_3d_gun_designs)). Gun files move to Odysee and specialist sites rather than vanishing. Any design that is censorship-resistant will be judged against this.
5. **Theft and resale.** Free or paid models get re-uploaded as "FREE TO SELL" and resold on Etsy, eBay and Goofish, often together with the stolen photos ([forum](https://forum.bambulab.com/t/user-reuploading-exclusive-models-with-free-to-sell-in-title/142888); [forum](https://forum.bambulab.com/t/you-need-to-check-out-etsy-regularly/139641?page=4)). Detection is manual and done by the platform.
6. **AI slop.** Text→image→3D models get uploaded with AI cover images that don't match the file ([Fabbaloo](https://www.fabbaloo.com/?p=234795)). Responses differ: labels and filters (Thingiverse 2025, Printables), real-print-photo rules (MakerWorld), and outright bans (MMF/Thingiverse 2026). Separately, scraping tools for Thingiverse are sold openly ([Apify](https://apify.com/automation-lab/thingiverse-scraper.md)), and no platform offers a machine-readable "no AI training" signal (CC Signals is emerging) **[?]**.
7. **Licensing confusion.** The "Standard Digital File License" went MMF → Printables → MakerWorld with divergent wording. A lawyer notes that read literally, giving a printed toy away violates it ([forum, 2026-08](https://forum.bambulab.com/t/understanding-makerworlds-standard-digital-file-license/257609)). CC-NC is the de facto default, and nobody enforces it consistently against Etsy sellers.
8. **Opaque and regional payouts.** The MakerWorld points formula is hidden (2025) and only Exclusive points cash out. Payouts depend on PayPal, Stripe or SWIFT, with country gaps, FX losses and minimum thresholds.
9. **Quality and reliability.** Thingiverse's search and uptime problems are why users moved to Printables and MakerWorld. Discovery quality is a competitive moat.

---

## 3. Existing decentralized / open attempts

- **Manyfold** (open source, self-hosted, Rails).
  - Organizes your own model library, with browser 3D previews, multi-user permissions and **ActivityPub federation** via Federails. It was presented at FOSDEM 2025 and funded by NLnet NGI0 Commons for Jan 2025–Jan 2026. Planned work covers creator support, discovery and import/export ([NLnet](https://nlnet.nl/project/Manyfold-Discovery/); [FOSDEM](https://archive.fosdem.org/2025/schedule/event/fosdem-2025-4181-manyfold-federating-3d-models/)).
  - Why it hasn't replaced anything: its strength is a *personal library and archive*. It has no global discovery, payments or slicer one-click, and it needs a server. It is the closest prior art and a likely interop target (ActivityPub/import).
- **Pod21:** a Nostr + Lightning print-on-demand marketplace. NIP-07 is the identity, NIP-17 DMs handle messaging, payment goes through BTCPay, and the backend slices with a dockerized OrcaSlicer ([damus/nostr](https://damus.io/npub1l5pxvjzhw77h86tu0sml2gxg8jpwxch7fsj6d05n7vuqpq75v34syk4q0n)) **[?]** (I only saw the description; adoption is unknown). It is about printing services, not a model repository.
- **Web3/NFT marketplaces:**
  - Examples: Web3DP (ERC-721 plus NFT.storage on IPFS/Filecoin), academic "3D Marketplace" attestation ([arXiv 1908.06921](https://arxiv.org/abs/1908.06921)), and Iagon×Würth IP-royalty NFTs (a Catalyst proposal).
  - Why they failed: STLs are bulky, and the NFTs usually point at centrally hosted URLs ([Fabbaloo](https://fabbaloo.com/?p=200433)). Owning a token is not a print license. Buyers pay gas fees, the products aimed at speculation rather than makers, and nobody built search or a community.
- **The Pirate Bay "Physibles"** (2012) ([PopSci](https://www.popsci.com/technology/article/2012-01/get-your-3-d-printer-models-pirate-bay)) and gun-file distribution over Odysee/LBRY. These show that p2p distribution works for *contested* content. They also tie the "decentralized 3D files" brand to piracy and weapons, which is a reputational risk to plan for.
- **OctoPrint plugins / Thingiverse API apps** ([Thingiverse apps](https://www.thingiverse.com/apps)): useful clients, but still dependent on the central API.
- **General lesson:** no attempt so far combines (a) durable content-addressed files, (b) usable discovery, (c) creator money and (d) slicer integration. Each covers one or two.

---

## 4. Requirements for an MVP decentralized "Printables-like" app

### MUST
1. **Content-addressed model bundles.** A signed manifest lists files (STL, 3MF, STEP/source, OBJ), images and a README by CID. The model ID stays stable while versions are immutable, giving real version history (no centralized site offers this today).
2. **Creator identity plus signatures** on every model and version. This underpins attribution and remix chains, and lets a creator delete *their* index entry while CIDs persist.
3. **Explicit, machine-readable license** on every model: SPDX/CC IDs plus a small set of clear custom templates (personal-use, commercial-print-OK, attribution required), and an AI-training flag.
4. **Remix links** (`derivedFrom: [modelId@version]`) shown as a tree.
5. **Makes, comments and ratings** as signed append-only events on p2p messaging (CRDT fold).
6. **Basic discovery:** tags/categories, full-text search over a locally synced index, sort by recent or by makes.
7. **Pinning and availability:** creators and followers pin, plus at least one always-on hub or cache. Show availability status ("seeded by N"). A model that can't be fetched is worse than link rot.
8. **Local-first client blocklist and moderation:** users subscribe to curated block or allow lists. There is no global delete, but every client must hide illegal content by default.
9. **Open slicer handoff:** download a STEP/3MF bundle or "open in" via OS handler. Profiles are labelled with their slicer and printer, with no cloud dependency.

### SHOULD
- Collections/lists as signed, shareable objects. Follow creators.
- One-off paid models via the ZK zone: pay → receive a decryption key for an encrypted bundle. Accept that a buyer can re-share it, as already happens on every platform.
- Tips and recurring support (Clubs/Tribes-style) with fixed, public, low protocol fees.
- Several print profiles per model contributed by others (as on MakerWorld), each signed and credited.
- In-browser/in-app 3D preview and thumbnails generated client-side.
- Import from Printables, Thingiverse and Manyfold (ActivityPub). This solves the cold-start problem by letting creators mirror their back catalogue.
- AI-generated label plus a "real print photo" convention.

### LATER
- Geometric/shape similarity search, which needs embeddings and a crawl. It also helps detect stolen re-uploads.
- Reputation-weighted ranking and curator-run "front pages".
- Contests and bounties, crowdfunding (escrow in the ZK zone).
- Direct-to-printer (OctoPrint/Moonraker/PrusaLink LAN plugins).
- Commercial license marketplace (print-and-sell rights).
- Parametric/customizer models (OpenSCAD) run locally.

### Genuinely hard without a company
- **Moderation and legal takedowns:** DMCA/EU notice-and-action, firearms (Manhattan DA-style pressure), CSAM, and trademark (GW/LEGO). Without a central host there is no one to send notices to, yet regulators and app stores will demand an answer. Curated lists and hub operators become the de facto moderators.
- **Search ranking and anti-spam:** good ranking needs global signals (downloads, makes) that are easy to sybil without identity cost. Spam and AI slop flood open indices.
- **Stolen-model and fraud detection:** platforms do it manually (MakerWorld's ~4,000 cases). A p2p network can't adjudicate who the "real" author is. At best: first-seen timestamps, signatures, geometric-hash similarity flags.
- **Chargebacks and refunds:** card networks impose them. Crypto/ZK payments avoid chargebacks but put fraud and dispute handling on the buyer.
- **Fiat payouts and taxes:** KYC, VAT/sales-tax collection (EU digital goods), 1099/DAC7 reporting, and PayPal/bank rails per country. That is why everyone uses Stripe/PayPal. A ZK-token payout leaves creators with an off-ramp problem.
- **Guaranteed availability and performance:** CDN-backed 100–250 MB downloads versus pinning by volunteers.
- **Incentive budgets:** Prusameters, MakerWorld points and contests are subsidised by hardware margins. A protocol has no such margin to fund them.
- **Slicer and printer vendor partnerships:** one-click print exists because Prusa and Bambu own both ends.
