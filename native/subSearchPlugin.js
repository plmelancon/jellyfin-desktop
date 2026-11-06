/**
 * SubSearch - search and download subtitles from inside the video player.
 * Adds a button to the player controls; results come from the server's
 * subtitle providers (OpenSubtitles plugin). After download, playback
 * restarts at the same position with the new subtitle selected.
 * Bundled in the desktop client so it works on any server with a subtitle
 * provider plugin (e.g. OpenSubtitles); it does nothing
 * if the server already injects the same button (.btnSubSearch).
 */
(function () {
  'use strict';
  const TAG = '[SubSearch]';
  const LANGS = [['eng', 'English'], ['fre', 'Français']];
  let dlg = null;

  const esc = s => String(s == null ? '' : s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  const sleep = ms => new Promise(r => setTimeout(r, ms));

  async function api(path, method) {
    const c = window.ApiClient;
    const r = await fetch(c.getUrl(path), {
      method: method || 'GET',
      headers: { Authorization: `MediaBrowser Token="${c.accessToken()}"` }
    });
    if (!r.ok) throw new Error(`${r.status} on ${path}`);
    const t = await r.text();
    return t ? JSON.parse(t) : null;
  }

  async function currentSession() {
    const list = await api('Sessions?DeviceId=' + encodeURIComponent(window.ApiClient.deviceId()));
    const s = (list || []).find(x => x.NowPlayingItem) || null;
    if (!s) throw new Error('No item playing on this device');
    return s;
  }

  async function getItem(id) {
    return api(`Items/${id}?userId=${window.ApiClient.getCurrentUserId()}`);
  }

  const subStreams = item => ((item.MediaSources && item.MediaSources[0] && item.MediaSources[0].MediaStreams) || item.MediaStreams || [])
    .filter(s => s.Type === 'Subtitle');

  function close() {
    if (!dlg) return;
    dlg.backdrop.remove();
    dlg.container.remove();
    dlg = null;
  }

  function setBody(html) {
    if (dlg) dlg.body.innerHTML = html;
  }

  function open() {
    close();
    const backdrop = document.createElement('div');
    backdrop.className = 'dialogBackdrop dialogBackdropOpened';
    backdrop.style.cssText = 'z-index:1000;';
    const container = document.createElement('div');
    container.className = 'dialogContainer';
    container.style.cssText = 'z-index:1001;';
    const box = document.createElement('div');
    box.className = 'focuscontainer dialog opened';
    box.style.cssText = 'position:fixed;left:50%;top:50%;transform:translate(-50%,-50%);width:min(720px,94vw);max-height:80vh;display:flex;flex-direction:column;padding:1em 1.2em;border-radius:.5em;';
    box.innerHTML = `
      <div style="display:flex;align-items:center;gap:.6em;margin-bottom:.6em">
        <h3 style="margin:0;flex:1">Search subtitles</h3>
        ${LANGS.map(([c, n]) => `<button is="emby-button" class="raised emby-button subsearch-lang" data-lang="${c}" style="margin:0">${n}</button>`).join('')}
        <button is="paper-icon-button-light" class="paper-icon-button-light subsearch-close" title="Close"><span class="material-icons" aria-hidden="true">close</span></button>
      </div>
      <div class="subsearch-body" style="overflow-y:auto;flex:1;min-height:4em"></div>`;
    container.appendChild(box);
    document.body.appendChild(backdrop);
    document.body.appendChild(container);
    dlg = { backdrop, container, body: box.querySelector('.subsearch-body') };
    backdrop.addEventListener('click', close);
    container.addEventListener('click', e => { if (e.target === container) close(); });
    box.querySelector('.subsearch-close').addEventListener('click', close);
    box.querySelectorAll('.subsearch-lang').forEach(b => b.addEventListener('click', () => search(b.dataset.lang)));
    let last = 'eng';
    try { last = localStorage.getItem('subsearch-lang') || 'eng'; } catch (e) { /* ignore */ }
    search(last);
  }

  async function search(lang) {
    try { localStorage.setItem('subsearch-lang', lang); } catch (e) { /* ignore */ }
    setBody('<p>Searching…</p>');
    try {
      const s = await currentSession();
      const item = s.NowPlayingItem;
      const res = await api(`Items/${item.Id}/RemoteSearch/Subtitles/${lang}`);
      if (!dlg) return;
      if (!res || !res.length) { setBody('<p>No subtitles found for this language.</p>'); return; }
      res.sort((a, b) => (b.IsHashMatch === true) - (a.IsHashMatch === true) || (b.DownloadCount || 0) - (a.DownloadCount || 0));
      setBody(res.slice(0, 40).map((r, i) => `
        <button is="emby-button" type="button" class="listItem listItem-button emby-button subsearch-res" data-i="${i}" style="width:100%;text-align:left">
          <div class="listItemBody">
            <div class="listItemBodyText">${esc(r.Name)}</div>
            <div class="listItemBodyText secondary">${esc(r.ProviderName)} · ${esc(r.Format || '')} · ${r.DownloadCount || 0} downloads${r.IsHashMatch ? ' · <b>exact match</b>' : ''}${r.HearingImpaired ? ' · SDH' : ''}${r.Comment ? ' · ' + esc(r.Comment) : ''}</div>
          </div>
        </button>`).join(''));
      dlg.body.querySelectorAll('.subsearch-res').forEach(b => b.addEventListener('click', () => download(res[+b.dataset.i], lang)));
    } catch (e) {
      console.error(TAG, e);
      setBody(`<p>Search failed: ${esc(e.message)}</p>`);
    }
  }

  async function download(result, lang) {
    setBody('<p>Downloading…</p>');
    try {
      const s = await currentSession();
      const itemId = s.NowPlayingItem.Id;
      const before = await getItem(itemId);
      const beforeIdx = new Set(subStreams(before).map(x => x.Index));
      await api(`Items/${itemId}/RemoteSearch/Subtitles/${encodeURIComponent(result.Id)}`, 'POST');
      setBody('<p>Downloaded. Waiting for Jellyfin to register the file…</p>');

      let added = null;
      for (let i = 0; i < 30 && !added; i++) {
        await sleep(1000);
        if (i === 6) { try { await api(`Items/${itemId}/Refresh?metadataRefreshMode=Default&imageRefreshMode=None`, 'POST'); } catch (e) { /* non-admin */ } }
        const now = await getItem(itemId);
        const fresh = subStreams(now).filter(x => !beforeIdx.has(x.Index));
        added = fresh.find(x => x.IsExternal) || fresh[0] || null;
      }
      if (!added) {
        setBody('<p>Subtitle saved, but Jellyfin has not picked it up yet. Stop and reopen the episode, then choose it under CC.</p>');
        return;
      }

      // Restart playback at the current position with the new track selected.
      const s2 = await currentSession();
      const pos = (s2.PlayState && s2.PlayState.PositionTicks) || 0;
      const msId = (s2.PlayState && s2.PlayState.MediaSourceId) || '';
      const q = new URLSearchParams({
        playCommand: 'PlayNow', itemIds: itemId, startPositionTicks: String(pos), subtitleStreamIndex: String(added.Index)
      });
      if (msId) q.set('mediaSourceId', msId);
      if (s2.PlayState && s2.PlayState.AudioStreamIndex != null) q.set('audioStreamIndex', String(s2.PlayState.AudioStreamIndex));
      close();
      await api(`Sessions/${s2.Id}/Playing?${q}`, 'POST');
    } catch (e) {
      console.error(TAG, e);
      setBody(`<p>Download failed: ${esc(e.message)}</p>`);
    }
  }

  function makeButton() {
    const b = document.createElement('button');
    b.setAttribute('is', 'paper-icon-button-light');
    b.className = 'btnSubSearch autoSize paper-icon-button-light';
    b.title = 'Search subtitles';
    b.setAttribute('aria-label', 'Search subtitles');
    b.innerHTML = '<span class="xlargePaperIconButton material-icons" aria-hidden="true">subtitles</span>';
    b.addEventListener('click', e => { e.stopPropagation(); open(); });
    return b;
  }

  function addButton() {
    if (!location.hash.startsWith('#/video')) { close(); return; }
    if (document.querySelector('.btnSubSearch')) return;
    const bar = document.querySelector('.videoOsdBottom .buttons.focuscontainer-x');
    if (!bar) return;
    const anchor = bar.querySelector('.btnJellysleep') || bar.querySelector('.btnUserRating');
    const btn = makeButton();
    if (anchor) anchor.insertAdjacentElement('beforebegin', btn); else bar.appendChild(btn);
  }

  function init() {
    if (!document.body || !(window.ApiClient && window.ApiClient.accessToken && window.ApiClient.accessToken())) {
      setTimeout(init, 1000);
      return;
    }
    new MutationObserver(() => { try { addButton(); } catch (e) { console.error(TAG, e); } })
      .observe(document.body, { childList: true, subtree: true });
    addButton();
  }

  try { init(); } catch (e) { console.error(TAG, e); }
})();
