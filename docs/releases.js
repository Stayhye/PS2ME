// Fetches the project's releases straight from the GitHub REST API and renders
// them: the newest as a featured card, the rest as a collapsible history.
// Each release's notes (its per-version changelog) are Markdown, rendered with
// marked (loaded from CDN). Fails gracefully to a link if the API is
// unreachable or the repo is still private (anonymous requests get a 404).
(function () {
  'use strict';

  var root = document.getElementById('releases-root');
  if (!root) return;
  var repo = root.getAttribute('data-repo');
  var API = 'https://api.github.com/repos/' + repo + '/releases';
  var RELEASES_URL = 'https://github.com/' + repo + '/releases';

  // ---- helpers -----------------------------------------------------------
  function el(tag, cls, html) {
    var node = document.createElement(tag);
    if (cls) node.className = cls;
    if (html != null) node.innerHTML = html;
    return node;
  }

  function esc(s) {
    return String(s).replace(/[&<>"']/g, function (c) {
      return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c];
    });
  }

  function fmtDate(iso) {
    if (!iso) return '';
    var d = new Date(iso);
    if (isNaN(d)) return '';
    return d.toLocaleDateString(undefined, { year: 'numeric', month: 'short', day: 'numeric' });
  }

  function fmtSize(bytes) {
    if (!bytes && bytes !== 0) return '';
    if (bytes < 1024) return bytes + ' B';
    if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(0) + ' KB';
    return (bytes / (1024 * 1024)).toFixed(1) + ' MB';
  }

  function renderNotes(md) {
    if (!md) return '<p class="rel-empty">No release notes.</p>';
    if (typeof marked !== 'undefined') {
      return marked.parse(md, { mangle: false, headerIds: false });
    }
    return '<pre>' + esc(md) + '</pre>';   // marked blocked: show raw notes
  }

  function endsWith(name, suffix) {
    var s = String(name || '').toLowerCase();
    return s.slice(s.length - suffix.length) === suffix;
  }

  // History items: a plain list of every downloadable asset.
  function assetList(assets) {
    var real = (assets || []).filter(function (a) { return a && a.browser_download_url; });
    if (!real.length) return null;
    var wrap = el('div', 'rel-assets');
    real.forEach(function (a) {
      var link = el('a', 'rel-asset');
      link.href = a.browser_download_url;
      link.innerHTML = '<span class="rel-asset-name">' + esc(a.name) + '</span>' +
        '<span class="rel-asset-size">' + esc(fmtSize(a.size)) + '</span>';
      wrap.appendChild(link);
    });
    return wrap;
  }

  // Featured (latest) release: the .zip is the big button; everything else
  // (the standalone .elf, etc.) becomes a small secondary link on the side.
  function featuredAssets(assets) {
    var real = (assets || []).filter(function (a) { return a && a.browser_download_url; });
    if (!real.length) return null;

    var zip = real.filter(function (a) { return endsWith(a.name, '.zip'); })[0];
    var elf = real.filter(function (a) { return endsWith(a.name, '.elf'); })[0];
    var primary = zip || elf || real[0];               // prefer the complete .zip
    var alts = real.filter(function (a) { return a !== primary; });

    var wrap = el('div', 'rel-assets');

    var big = el('a', 'rel-asset rel-asset-primary');
    big.href = primary.browser_download_url;
    big.innerHTML = '<span class="rel-asset-name">Download ' + esc(primary.name) + '</span>' +
      '<span class="rel-asset-size">' + esc(fmtSize(primary.size)) + '</span>';
    wrap.appendChild(big);

    if (alts.length) {
      var side = el('span', 'rel-asset-alts');
      alts.forEach(function (a) {
        var link = el('a', 'rel-asset-alt');
        link.href = a.browser_download_url;
        var label = endsWith(a.name, '.elf') ? 'ELF only' : a.name;
        link.innerHTML = '<span class="rel-asset-alt-name">' + esc(label) + '</span> ' +
          '<span class="rel-asset-size">' + esc(fmtSize(a.size)) + '</span>';
        side.appendChild(link);
      });
      wrap.appendChild(side);
    }
    return wrap;
  }

  // Point the hero "DOWNLOAD the .ZIP!" button straight at the latest .zip
  // (the filename carries the version, so there is no static URL for it).
  function wireHeroDownload(rel) {
    var btn = document.getElementById('hero-download');
    if (!btn) return;
    var zip = (rel.assets || []).filter(function (a) {
      return a && a.browser_download_url && endsWith(a.name, '.zip');
    })[0];
    if (zip) btn.href = zip.browser_download_url;
  }

  function versionLabel(rel) {
    return rel.tag_name || rel.name || 'release';
  }

  // Order releases newest-version-first. The GitHub list API sorts by
  // created_at, which is wrong when older tags are published (backfilled)
  // later than newer ones — so sort by semantic version instead.
  function versionKey(rel) {
    return String(rel.tag_name || rel.name || '')
      .replace(/^v/i, '').split('.').map(function (n) { return parseInt(n, 10) || 0; });
  }
  function compareVersionsDesc(a, b) {
    var A = versionKey(a), B = versionKey(b);
    var len = Math.max(A.length, B.length);
    for (var i = 0; i < len; i++) {
      var diff = (B[i] || 0) - (A[i] || 0);
      if (diff) return diff;
    }
    return 0;
  }

  // ---- rendering ---------------------------------------------------------
  function renderFeatured(rel) {
    var card = el('article', 'rel-featured');

    var head = el('div', 'rel-featured-head');
    var badges = rel.prerelease ? '<span class="rel-badge rel-badge-pre">Pre-release</span>' : '';
    head.innerHTML =
      '<div class="rel-featured-title">' +
        '<span class="rel-badge rel-badge-latest">Latest</span>' + badges +
        '<h3>' + esc(rel.name || versionLabel(rel)) + '</h3>' +
        '<span class="rel-tag">' + esc(versionLabel(rel)) + '</span>' +
      '</div>' +
      '<span class="rel-date">' + esc(fmtDate(rel.published_at)) + '</span>';
    card.appendChild(head);

    var assets = featuredAssets(rel.assets);
    if (assets) card.appendChild(assets);

    card.appendChild(el('div', 'rel-notes', renderNotes(rel.body)));

    var foot = el('p', 'rel-featured-foot',
      '<a href="' + esc(rel.html_url) + '">View this release on GitHub &rarr;</a>');
    card.appendChild(foot);
    return card;
  }

  function renderHistoryItem(rel) {
    var item = el('details', 'rel-history-item');
    var summary = el('summary');
    summary.innerHTML =
      '<span class="rel-history-tag">' + esc(versionLabel(rel)) + '</span>' +
      '<span class="rel-history-name">' + esc(rel.name || '') + '</span>' +
      '<span class="rel-date">' + esc(fmtDate(rel.published_at)) + '</span>';
    item.appendChild(summary);

    var body = el('div', 'rel-history-body');
    var assets = assetList(rel.assets);
    if (assets) body.appendChild(assets);
    body.appendChild(el('div', 'rel-notes', renderNotes(rel.body)));
    body.appendChild(el('p', 'rel-featured-foot',
      '<a href="' + esc(rel.html_url) + '">View on GitHub &rarr;</a>'));
    item.appendChild(body);
    return item;
  }

  function render(releases) {
    root.innerHTML = '';
    wireHeroDownload(releases[0]);
    root.appendChild(renderFeatured(releases[0]));

    var rest = releases.slice(1);
    if (rest.length) {
      root.appendChild(el('h3', 'rel-history-heading', 'Previous releases'));
      var list = el('div', 'rel-history');
      rest.forEach(function (rel) { list.appendChild(renderHistoryItem(rel)); });
      root.appendChild(list);
    }
  }

  function fallback(msg) {
    root.innerHTML =
      '<p class="rel-status">' + esc(msg) + '</p>' +
      '<p><a class="btn btn-primary" href="' + RELEASES_URL + '">Browse releases on GitHub</a></p>';
  }

  // ---- fetch -------------------------------------------------------------
  fetch(API, { headers: { Accept: 'application/vnd.github+json' } })
    .then(function (res) {
      if (!res.ok) throw new Error('HTTP ' + res.status);
      return res.json();
    })
    .then(function (data) {
      var releases = (data || []).filter(function (r) { return !r.draft; });
      if (!releases.length) {
        fallback('No published releases yet.');
        return;
      }
      releases.sort(compareVersionsDesc);   // newest version first, not newest-created
      render(releases);
    })
    .catch(function () {
      fallback('Could not load releases from GitHub right now.');
    });
})();
