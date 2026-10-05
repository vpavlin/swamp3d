# 03 — LEZ / Logos blockchain for payments + publication anchoring

Research date 2026-10-05. Sources are local clones under `/tmp/claude-1000/lez-research/` (LEZ at
`c31a6bc`, 2026-10-01; tag `v0.3.0` = `db66590`, 2026-09-29), the Bedrock (Logos blockchain) crates in
`~/.cargo/git/checkouts/logos-blockchain-2d93fcb89448de89/e2a1c3b`, the Basecamp
`blockchain_module` 0.2.4 in `~/blockchain_module-0.2.4-backup-20261001`, and live calls to the
public testnet RPC `https://testnet.lez.logos.co/`. Prior notes:
`~/.claude/projects/-home-vpavlin/memory/lez-token-gating-research.md`.

## 0. Mental model (v0.3)

- **Bedrock / Logos blockchain (L1)**: Cryptarchia consensus plus "Mantle", a small execution layer. Its
  key primitive for us is the **channel**: a permissioned, ordered log of **inscriptions** (`ChannelInscribe` op).
- **LEZ** is a *sovereign zone* built on one Bedrock channel. Its sequencer publishes every L2 block as a
  single inscription (`lez/sequencer/core/src/config.rs`: `MAX_PUBLISHABLE_BLOCK_SIZE =
  inscribe::MAX_BYTES`). LEZ runs RISC Zero programs over accounts.
- **v0.3 account model** (changed from v0.2.4): an account is `{balance, nonce, shards}`. Each program
  owns *its own shard* on any account. "A program can modify its own shard on any account". Debiting a
  balance needs authorization: a Schnorr signature for a public account, or knowledge of `ask` for a
  private one. Programs may also choose to require authorization before writing their shard
  (`examples/program_deployment/README.md` §8, §13). The PDAs are bound to an authority program + seed.
  Account data is capped at `DATA_MAX_LENGTH = 100 KiB` (`lee/state_machine/core/src/account/data.rs`).
- Programs use a **plan/apply** split and can emit **events** `ProgramEvent{selector:[u8;8], data}`
  (`lee/state_machine/core/src/program/mod.rs:549`). The indexer serves these through `getEvents` /
  `subscribeToEvents`, filtered by program, selector and tx hash (`lez/indexer/service/rpc/src/lib.rs`).

## 1. Payments

### 1.1 What exists today
- **Native token** (the LEZ gas token): a built-in program at `NATIVE_TOKEN_PROGRAM_ID = [0;32]` with a
  single `Transfer{amount: u128}` (`lee/state_machine/core/src/native_token.rs`). PDAs can pay out with
  `custody_transfer(from, seed, to, amount)`, which is how the fee program empties its escrow PDAs
  (`lez/programs/fee/src/main.rs`).
- **Fungible/NFT tokens**: the `token` program (now in-tree at `lez/programs/token`, and deployed on
  testnet; `getProgramIds` returns `amm`, `token`, `privacy_preserving_circuit`). Holdings live in ATAs.
  The v0.2.4 `lez-programs` repo copy is stale (pinned `v0.2.4`).
- **Basecamp `lez_core` 0.5.0** (`logos-execution-zone-module/src/lez_core_module.h`, v0.3-aware) gives
  another module (via `logos.callModule`) these calls:
  - accounts: `create_account_public/private`, `list_accounts`, `get_balance`
  - native transfers: `transfer_public`, `transfer_shielded`, `transfer_deshielded`, `transfer_private`,
    `*_owned`. Amounts are 16-byte LE hex.
  - arbitrary programs: `send_generic_public_transaction(account_ids, signing_requirements,
    borsh_instruction, program_id, payer, shard_selectors)` and `send_generic_private_transaction(...)`
  - `send_program_deployment_transaction` (ELF split into 96 KiB segments)
  - `poll_transaction_status(tx_hash)`, which returns only a bool (found or not)
  - labels and bridge withdraw.
  - **No arbitrary message signing.** `lez_core` only signs transactions.
- **`lez_indexer_module` 1.2.0**: `getTransaction`, `getTransactionsByAccount`, `getAccount`,
  `getBlock*`, `getLastFinalizedBlockId`. It does **not** wrap `getEvents` yet (the RPC has it), and it
  needs an L1 node. `lez_wallet_ui` 1.2.0 is the stock Basecamp wallet view, built on `lez_core >= 0.5.0`.

### 1.2 Cost and latency
- The fee market has two dimensions (`lez/programs/fee/core/src/market.rs`): execution gas and storage
  gas, each with an EIP-1559-style base fee (minimum 8, ±12.5 %/block).
  - Storage gas for a public tx = **its serialized bytes** (`assess.rs::gas_stor`). Public txs are
    ~160–250 B.
  - A private tx is charged a fixed ~225 KB proof-size storage gas plus `PRIVATE_VERIFY_GAS = 409,764`
    execution gas.
  - Live testnet `getFeeState`: `base_fee_exec=8, base_fee_stor=8, max_gas_stor=1,000,000/block`. So a
    public payment costs on the order of a few thousand base units. A private one is roughly 1000× more
    in storage gas.
- **Latency.** On testnet I observed 48 blocks in 30 min (≈37 s/block; blocks 10900→10948, decoded
  timestamps). Benchmarks (`docs/benchmarks/integration_bench.md`) put public tx inclusion at ≈11 s
  locally, and Bedrock finality takes another 6–32 s. Private transfers need ≈125 s of **client-side
  proving per step** on an M2 Pro CPU, with no mobile prover.
- **Testnet only.** Tokens come from the faucet (150 testnet LEZ per claim, `lez-faucet`). No mainnet and
  no fiat/stablecoin rail. A `stablecoin` program exists in lez-programs, but only for v0.2.4.

### 1.3 Proving payment to the seller
- A public native transfer is visible to anyone. The seller app can call `getTransaction(hash)` (sequencer
  or indexer), decode it, and check program = native, sender, recipient and amount. To confirm finality
  it can check the block ≤ `getLastFinalizedBlockId`, or use `getTransactionsByAccount(seller)`.
- **Problem: a public tx `Message` has no memo field** (`lee/state_machine/src/public_transaction/message.rs`:
  program, selectors, nonces, instruction, fee). A plain transfer therefore can't say *which* model or
  which buyer key it pays for. The buyer can send the tx hash over Waku, but a passive observer could
  replay someone else's hash. Fixes:
  1. **A tiny `purchase` program (recommended).** It writes a receipt into its own shard and/or emits
     `Purchased{listing_id, buyer_msg_pubkey, amount}`, and **chains** a native `Transfer` from the signing
     buyer to the seller in the same tx. This exact pattern is in
     `lee/state_machine/test_methods/guest/src/bin/native_spender.rs` (write own shard + `ChainedCall` to
     the native token). The seller verifies with `getEvents{program_account_id, tx_hash}`. This is
     atomic, carries a memo and can't be replayed (the receipt binds the buyer's key).
  2. Per-order derived recipient addresses: clunky, and it fragments the seller's balance.
- Private transfers hide this from the indexer. The seller only sees an incoming private note after
  syncing its wallet, and there is no "prove I paid" receipt. Keep payments **public** for an MVP.

### 1.4 Pay-to-unlock / escrow (fair exchange)
A program can't keep a secret: all public state and inputs are visible. The decryption key therefore has
to move off-chain or be encrypted to the buyer. Options:

| Option | How | Trust | Feasible now |
|---|---|---|---|
| A. Seller-online release | Buyer pays through `purchase`. The seller's app (or the seller's always-on hub) watches `subscribeToEvents`, then sends `K_model` sealed to `buyer_msg_pubkey` over Waku/Loam. | Buyer trusts seller to deliver. Seller app must be online. A hub fixes this, but the hub then holds keys. | Yes, simplest |
| B. Encrypt-key-to-buyer on chain | The seller posts `Deliver{order, enc(K_model → buyer_pk)}` into the order PDA. The escrow pays out **only when `Deliver` happens**. | Buyer is protected from non-delivery, but **not** from a garbage ciphertext: the program can't check `enc(K)` decrypts the model. | Yes (ECIES blob ≈ 100 B) |
| C. Refund-after-timeout escrow | The buyer locks funds in an order PDA. The seller must `Deliver` before `deadline`, read from the clock program (`CLOCK_*_PROGRAM_ACCOUNT_ID` hold `{block_id,timestamp}`, `lez/programs/clock/core/src/lib.rs`). Otherwise the buyer calls `Refund`. Payout uses `custody_transfer` from the PDA. | Combines with B. Still only "something was delivered". | Yes |
| D. Arbiter / multisig dispute | Add `Dispute` → a 2-of-3 (buyer, seller, arbiter) resolves. `lez-multisig` (Squads-like, `ChainedCall` execute) is the template. It is pinned to `v0.2.0-rc3`, so it needs porting. | Arbiter trust | Port needed |
| E. Verifiable delivery (ZK) | A RISC0 proof that `enc(K→buyer)` decrypts `CID` and the result hashes to `contentHash`. | Trustless | Research. Proving a full STL decrypt costs too much in cycles. A commitment trick (prove `K` matches a `H(K)` published with the listing) is cheap and catches only "wrong key", not "bad model". |

The realistic MVP is **A, optionally upgraded to B+C**. This is a hostage problem the chain can't fully
solve: the buyer can always leak the model after paying. Licence enforcement is social or legal, not
cryptographic.

## 2. Anchoring publications

### (i) SPEL/LEZ "model registry" program
- Store `ModelVersion{model_id, version, content_hash, cid, author, licence, parent{model_id,version},
  ts}` in a PDA per (model_id, version), and the head pointer in a PDA per model_id. Also emit an event
  so indexers and the Basecamp view can stream new publications.
- Size: ~200–300 B per record, far below the 100 KiB shard cap. The cost is dominated by tx bytes:
  storage gas is charged on the **serialized tx**, not on state growth. On testnet that is a few thousand
  base units per publish, paid by the **author's** public account (`payer_account_id_hex`). A hub can
  sponsor publishes: the payer can be any key the wallet holds and co-signs.
- Timestamp: block timestamp, or read the clock account in-program. Ordering and finality come from the
  L2 block plus Bedrock finality.
- **Blocker: SPEL is pinned to LEZ `v0.2.4`** (`spel/spel-framework*/Cargo.toml`; CHANGELOG v0.7.0,
  2026-09-16). The testnet already runs **v0.3** (live `getFeeState` works). That means
  plan/apply plus shards, with no back-compat. Today a v0.3 program has to be written against raw
  `lee_core` the way `examples/program_deployment` does it, or you wait for a SPEL v0.3 port. A raw
  registry is still small: ~3 instructions, no CPI except optional fee-sponsor.

### (ii) Bedrock channel inscriptions / a dedicated zone
- `InscriptionOp{channel_id, inscription ≤ MAX_BYTES, parent: MsgId, signer: Ed25519}`
  (`core/src/mantle/ops/channel/inscribe.rs`).
  - `MAX_BYTES = 2 MiB × 7/8 ≈ 1.75 MiB`.
  - **The first inscription on a new channel id creates the channel**, with the signer as its sole
    accredited key.
  - After that, only accredited keys may write, in round-robin by slot. Each inscription must parent on
    the current tip, so the channel is a strictly ordered hash-chained log.
  - The fee is Mantle storage gas ∝ encoded bytes, paid from the tx's ledger inputs (L1 notes).
- **This fits "anchoring" better than LEZ does.** A channel *is* an append-only, timestamped,
  ordered log. There is no zkVM, no program to deploy, and no SPEL/v0.3 dependency. A "marketplace
  anchor" channel signed by one Ed25519 key (a hub) or a small committee
  (`zone-sdk/DECENTRALIZED_SEQUENCING.md`) gives global ordering. Each publication or batch root
  becomes one inscription.
- **What it takes today:**
  - an L1 node: the Basecamp `blockchain_module` 0.2.4 or a `logos-blockchain-node`
  - funded L1 notes
  - the Rust **zone-sdk** (`zone-sdk/src/{sequencer,indexer}`), with an HTTP adapter to a node.
  The Basecamp module does **not** expose an `inscribe` method. Its surface is `start/stop`, `get_block*`,
  `get_transaction`, `get_channel_state`, `channel_deposit*`, `wallet_transfer_funds`,
  `wallet_get_balance/notes`, `submit_signed_transaction`, `generate_key/add_key`
  (from `strings blockchain_module_plugin.so`). An inscription would have to be built and signed
  out-of-process and pushed through `submit_signed_transaction`, or done with a small Rust sidecar on
  zone-sdk.
- Downsides:
  - Ed25519, not secp256k1, so it doesn't match the creator's key and the hub signs on authors' behalf.
  - Readers need an L1 node or zone-sdk indexer.
  - Inscriptions aren't "verified state". Validity rules (author signature, no version rewrites) are
    enforced by clients folding the log, which is exactly how loam-sync already works.
  - A full "sovereign zone" (own sequencer, stake, fee program) is overkill for anchoring.

### (iii) Batching (recommended in either case)
- Publications stay in the loam-sync event log (signed by the author's secp256k1 key, sealed CRDT).
- Every epoch (e.g. 10 min or N events), a hub builds a Merkle tree over
  `H(signed_publication_event)` and anchors **one 32-byte root plus a count/epoch** in one LEZ registry
  tx or one Bedrock inscription.
- A creator gets an inclusion proof (log₂N hashes) as a portable "published-before-T" certificate.
  Cost per publication tends to zero, and authors need no wallet, no tokens and no proving.
- Trade-off: the hub can delay or censor, but it can't backdate. An author who cares can anchor
  individually.

## 3. Identity binding (LEZ key ↔ loam-sync secp256k1 ECDSA)
- LEZ public accounts use **secp256k1 BIP-340 Schnorr** (`lee/state_machine/src/signature/mod.rs`, k256;
  message pre-hashed to 32 B, "pre-2022 BIP-340/Keycard compatible"). Account id =
  SHA256(prefix ‖ x-only pk).
  - Same curve as loam-sync's ECDSA, but **wallet keys are tweaked** (sk + SHA256(cpk)), so the same
    seed does not give the same pubkey.
  - The wallet FFI does no message signing.
- Practical bindings:
  1. **The registry tx is the LEZ-side signature.** `RegisterCreator{loam_pubkey, sig_ecdsa}` is signed
     by the LEZ account (tx auth). The program, or clients, verify `sig_ecdsa` by `loam_pubkey` over
     `"bind:" ‖ lez_account_id ‖ chain/program id`. k256 is already a LEE dependency, and RISC0 has a
     k256 precompile, so in-guest verification is feasible.
  2. **Loam-side:** a loam-sync `CreatorProfile` event carries `lez_account_id` (signed ECDSA). Both
     directions together give a mutual binding.
  3. Keycard can do both ECDSA and BIP-340 on-card (`docs/LEZ testnet v0.1 tutorials/keycard.md`,
     `lez/keycard_wallet`), so a single card can hold both identities ([[keycard-loam-identity]]).
- Bedrock channel signers are **Ed25519**, a third key type. They only matter if the hub inscribes.

## 4. Maturity and risks
- **Runs on testnet now (v0.3):** native transfers (public and private), token/ATA, AMM, program
  deployment, events and indexer RPC. Testnet tip was block 10948 on 2026-10-05.
- **Back-compat:**
  - Testnet was **reset** for v0.3, and **v0.2.4 ↔ v0.3 are incompatible**: account layout,
    plan/apply program model, IDs.
  - Most ecosystem code is still on v0.2.x: SPEL (v0.2.4), lez-programs (v0.2.4), lez-multisig
    (v0.2.0-rc3), lez-faucet (Aug).
  - Expect further resets. Anything anchored on testnet is **not durable**, so it can't be a source of
    truth.
- **Missing:**
  - SPEL for v0.3
  - memo on transfers
  - message signing in wallet-ffi/lez_core
  - `getEvents` in `lez_indexer_module`
  - any JS/Android SDK and mobile proving
  - an `inscribe` call in the Basecamp blockchain_module
  - mainnet / real value
  - escrow or marketplace programs (none upstream)
- **Risky:**
  - The ~37 s blocks and ≈2 min private proofs make for bad purchase UX.
  - Blocking `lez_core` IPC from QML: use async from a core module ([[no-blocking-ipc-in-qml]]).
  - The SPEL "signer claims unowned account" trap: an un-initialized account first signed into a SPEL
    program becomes owned and can't send native tokens (SPEL CHANGELOG v0.7.0). This is v0.2.4
    semantics; recheck on v0.3.
  - Phones have no LEZ client, so mobile buyers would need a desktop/hub relay.

## 5. Recommendation for an MVP

**Off-chain (source of truth):** model files in Logos Storage (encrypted for paid models), metadata,
versions, remix lineage and licence as **author-signed loam-sync events**. Discovery and comments go
over Waku. Authorship already rests on secp256k1 signatures, so the chain only adds *time* and
*payment*.

**On LEZ:**
1. **Payments (phase 1):** public native-token payment through a small `purchase` program (receipt +
   chained transfer). Delivery is option A: the seller's core module or hub watches events and sends
   `K_model` sealed to the buyer's loam pubkey. All of it goes through `lez_core.send_generic_public_transaction`
   from the marketplace core module.
2. **Anchoring (phase 1):** a hub-run epoch **Merkle-root anchor** (one tx per epoch). Optional per-model
   registry entries for authors who want them. Treat anchors as advisory while testnet resets.
3. **Phase 2:** escrow B+C (deliver-or-refund with a clock deadline), then an arbiter via a ported
   multisig. Consider Bedrock inscriptions for the epoch anchor if the LEZ program toolchain lags.

### Program sketch: `model_registry` (raw lee_core v0.3 now, SPEL when ported)
```
Accounts / shards (registry program's shard):
  Creator PDA  seed=("creator", lez_author_id)      {loam_pubkey[33], bind_sig[64], created_ts}
  Model PDA    seed=("model", model_id[32])         {author lez_id, latest_version u32, licence u16}
  Version PDA  seed=("ver", model_id, version u32)  {content_hash[32], cid bytes(≤64), parent:(model_id,ver)?,
                                                      licence u16, ts u64}
  Epoch PDA    seed=("epoch", epoch u64)            {root[32], count u32, hub_id, ts}   (hub only)
Instructions:
  RegisterCreator{loam_pubkey, bind_sig}   signer=author; verify ECDSA in-guest; init Creator PDA
  Publish{model_id, version, content_hash, cid, licence, parent?}
      signer=author; Model PDA init-or-check author==signer; require version == latest+1 (immutable history);
      read clock account for ts; init Version PDA; emit event "registry::Published"
  AnchorEpoch{epoch, root, count}          signer ∈ hub allowlist (config PDA); emit "registry::Anchored"
Notes: model_id = H(author_lez_id ‖ nonce) so ids can't be squatted; remix parent is a pointer only
(lineage is unverifiable beyond "author claimed it"); licence = SPDX enum code.
```

### Program sketch: `market_escrow` (phase 2)
```
  Listing PDA seed=("listing", model_id)  {seller, price u128, key_commit = H(K_model), deliver_window}
  Order PDA   seed=("order", listing, buyer, nonce) {buyer, buyer_enc_pubkey, amount, deadline, state,
                                                    enc_key? (≤ ~128 B)}
  List{model_id, price, key_commit, window}        signer=seller (must be registry author)
  Buy{listing, buyer_enc_pubkey, nonce}            signer=buyer; chained native Transfer buyer → Order PDA;
                                                   deadline = clock.ts + window; emit "Ordered"
  Deliver{order, enc_key}                          signer=seller; store enc_key; custody_transfer
                                                   Order PDA → seller; emit "Delivered"
  Refund{order}                                    signer=buyer; require clock.ts > deadline && !delivered;
                                                   custody_transfer → buyer
  (Dispute/Resolve via 2-of-3 arbiter later)
```
Phase 1 `purchase` is just `Buy` with a direct chained transfer to the seller (no PDA custody, no
Deliver/Refund).

Sources: local paths above; testnet RPC `https://testnet.lez.logos.co/` (`getLastBlockId`,
`getProgramIds`, `getFeeState`, `getBlock`); Bedrock architecture overview
https://nomos-tech.notion.site/v1-2-Bedrock-Architecture-Overview-2e0261aa09df80938c08cd51b9759788
("channels are permissioned, ordered logs of messages known as Inscriptions, signed by an authorized
party called the sequencer and stored permanently in-ledger").
