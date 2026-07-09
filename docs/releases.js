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

  function versionLabel(rel) {
    return rel.tag_name || rel.name || 'release';
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

    var assets = assetList(rel.assets);
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
      render(releases);
    })
    .catch(function () {
      fallback('Could not load releases from GitHub right now.');
    });
})();
