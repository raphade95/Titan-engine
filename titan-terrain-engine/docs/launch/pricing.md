# Pricing & Tiering (decision doc)

## Recommended structure

| Tier | Product | Price | Role |
|---|---|---|---|
| Free | **Titan Web Lab** (browser) | $0 | Funnel + demo. Full feature set, resolution capped at 512, watermark-free exports. |
| Paid | **TitanLab for macOS** | **$29 one-time** (intro $19) | The core product. Native Metal speed, resolutions to 4K (post-GPU update), file associations, priority updates. |
| Paid | **TitanBridge for Unreal** (Fab) | **$24.99** | In-editor generation for UE teams; reads .titan files from the other two. |

One-time pricing, not subscription: the competition context is Gaea
($99+ tiers, Windows-only) and World Machine ($119+, Windows-only) versus
Houdini ($269+/yr). Titan's wedge is *cheap, simple, Mac-first* — a $29
impulse price under Gaea's cheapest tier reinforces exactly that story.
Paid major upgrades (v2) later beat subscription resentment at this price
point.

## Why each tier exists

- **Web free**: zero-install proof of quality; every exported heightmap and
  shared .titan file advertises the paid tiers. Resolution cap is the
  upgrade pressure, not feature removal — crippled free tiers read as
  distrust.
- **Mac app paid**: the audience with no native alternative; the tier with
  real willingness to pay.
- **Fab plugin paid separately**: different buyer (often a team), Epic
  handles licensing/tax/refunds, and the marketplace is its own discovery
  channel. Cross-promote: plugin listing links the Mac app and vice versa.

## Payment rails

- **TitanLab**: Gumroad or Paddle (both handle EU VAT + license keys).
  Paddle scales better; Gumroad ships faster. Start Gumroad, move if volume
  justifies it.
- **TitanBridge**: Fab checkout (Epic's cut applies — factor ~12% + payment
  fees into the $24.99).

## Launch sequencing

1. Web lab public + free from day one (it's the marketing site's hero).
2. TitanLab at intro pricing for the first month ($19 → $29).
3. TitanBridge submitted to Fab once TitanLab has a week of stable telemetry.

## Open questions (revisit post-launch)

- Bundle price (Mac app + plugin) once both are live — likely $39.
- Team/site licensing for studios (5+ seats) — wait for actual demand.
- GPU-accelerated 8K tier as a paid v2 upgrade.
