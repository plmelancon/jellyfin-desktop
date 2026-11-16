# Fork Flatpak bundle

`.github/workflows/plm-flatpak.yml` builds `org.jellyfin.JellyfinDesktop` (branch `stable`, same as Flathub)
from the checked-out tree with `flatpak-builder` in the KDE 6.11 container, and uploads one
`jellyfin-desktop-plm-<shortsha>.flatpak` artifact. The CI job writes `<VERSION>-plm.<sha>` into the
`VERSION` file of its own checkout, so the log line "Starting Jellyfin version:" shows the commit.

Manifest: `org.jellyfin.JellyfinDesktop.yml` is a pinned copy of the Flathub manifest (flathub commit
72a7bd08a926, 2026-11). Fork edits: app source is `type: dir` (this tree); app config adds
`-DCHECK_FOR_UPDATES=OFF -DREQUIRE_KWIN_OUTPUT_MANAGEMENT=ON`.
Refresh: `gh api repos/flathub/org.jellyfin.JellyfinDesktop/contents/org.jellyfin.JellyfinDesktop.yml --jq .content | base64 -d`,
re-apply the two edits, update the commit id in the header comment.

Install on the HTPC (stop the app and back up the profile first; user-level ref shadows the system Flathub one):

    flatpak install --user --bundle jellyfin-desktop-plm-<shortsha>.flatpak
    flatpak run --user org.jellyfin.JellyfinDesktop//stable

Rollback:

    flatpak uninstall --user org.jellyfin.JellyfinDesktop//stable
    (full ref: app/org.jellyfin.JellyfinDesktop/x86_64/stable; the system install then runs again)
