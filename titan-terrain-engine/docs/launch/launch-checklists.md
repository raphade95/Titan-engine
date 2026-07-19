# Launch Checklists

Items marked **[HUMAN]** require accounts, payments, or identity that only
you can provide — everything else is scripted or already in the repo.

## A. Repository & CI

- [x] Git repository with phased history
- [x] CI workflow (mac/linux/windows core tests + web build)
- [ ] **[HUMAN]** Create GitHub repo and `git push` (CI activates on push)
- [ ] Confirm windows-latest job is green (proves the Fab Win64 story)

## B. Web lab (free tier)

- [x] Flat-canvas start, presets, layer stack, .titan save/load, exports
- [ ] **[HUMAN]** Choose + register domain (e.g. titanterrain.app)
- [ ] **[HUMAN]** Deploy: `npm run build` → any static host (Netlify /
      Cloudflare Pages / GitHub Pages). No server needed.
- [ ] Point the marketing site's "Launch the Lab" button at it

## C. TitanLab for macOS (paid tier)

- [x] App builds, smoke test passes, ad-hoc signed
- [ ] **[HUMAN]** Apple Developer Program ($99/yr) → Developer ID certificate
- [ ] **[HUMAN]** Re-sign + notarize (see notarization steps below)
- [ ] **[HUMAN]** Gumroad/Paddle product page + license keys
- [ ] Wrap in DMG (`hdiutil create -volname TitanLab -srcfolder build/TitanLab.app -ov -format UDZO TitanLab.dmg`)
- [ ] Private beta with 3–5 environment artists before public listing

### Notarization steps (once the Developer ID cert exists)

```bash
# 1. Re-sign with hardened runtime
codesign --force --options runtime --timestamp \
  --sign "Developer ID Application: YOUR NAME (TEAMID)" build/TitanLab.app
# 2. Zip and submit
ditto -c -k --keepParent build/TitanLab.app TitanLab.zip
xcrun notarytool submit TitanLab.zip --apple-id YOU --team-id TEAMID \
  --password APP_SPECIFIC_PW --wait
# 3. Staple the ticket
xcrun stapler staple build/TitanLab.app
```

## D. TitanBridge for Unreal (Fab)

- [x] Plugin source complete; compiled via BuildPlugin against UE 5.8
- [ ] Build `TitanCore.lib` on Windows CI; commit into
      `ThirdParty/TitanCore/lib/Win64/`
- [ ] **[HUMAN]** Epic/Fab publisher account + tax info
- [ ] **[HUMAN]** Fab listing: description, 5+ screenshots, demo video link,
      supported engine versions (5.3–5.8), price $24.99
- [ ] Package zip per Fab plugin guidelines (BuildPlugin output + README)
- [ ] Demo map screenshot set (use the Alpine preset at 512)

## E. Marketing

- [x] Marketing site (site/index.html — self-contained, deploy anywhere)
- [x] Tutorial video scripts (video-scripts.md)
- [ ] **[HUMAN]** Record the three videos (screen capture of web lab +
      TitanLab; scripts are timed at 60–90 s each)
- [ ] **[HUMAN]** Launch posts: r/proceduralgeneration, r/gamedev,
      r/unrealengine, X/Bluesky gamedev, Mac gaming Discords
- [ ] Positioning line everywhere: **"The terrain engine your Mac never had."**

## F. Support & feedback

- [ ] **[HUMAN]** support@ email or Discord server
- [ ] Crash reporting decision (recommend: none at launch; add opt-in later)
- [ ] Feedback link in both apps' About panels
