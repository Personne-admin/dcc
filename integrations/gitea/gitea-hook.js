(function () {
  'use strict';

  var LANGUAGE_CLASSES = ['language-dc', 'language-dcc', 'lang-dc', 'lang-dcc'];
  var HIGHLIGHT_CLASS = 'language-dc';
  var MARKER_ATTR = 'data-dc-highlighted';
  var SCRIPT_VERSION = '1.0.0';
  var DEBOUNCE_MS = 120;
  var PRISM_WAIT_MS = 100;
  var PRISM_WAIT_MAX_MS = 5000;

  function hasDcExtension(path) {
    if (!path)
      return false;

    var clean = String(path).split('?')[0].split('#')[0].toLowerCase();
    return clean.slice(-3) === '.dc';
  }

  function pageIsDcFile() {
    try {
      return hasDcExtension(window.location.pathname);
    } catch (e) {
      return false;
    }
  }

  function hasLanguageClass(el) {
    if (!el || !el.classList)
      return false;

    for (var i = 0; i < LANGUAGE_CLASSES.length; i++)
      if (el.classList.contains(LANGUAGE_CLASSES[i]))
        return true;

    return false;
  }

  function closestDataPath(el) {
    if (!el || !el.closest)
      return null;

    var box = el.closest('.diff-file-box');
    if (!box)
      return null;

    var title = box.querySelector('.file-header[data-path], [data-path]');
    if (!title)
      return box.getAttribute('data-path');

    return title.getAttribute('data-path');
  }

  function isDcBlock(el) {
    if (!el || el.hasAttribute(MARKER_ATTR))
      return false;

    if (hasLanguageClass(el))
      return true;

    if (pageIsDcFile())
      return true;

    if (hasDcExtension(closestDataPath(el)))
      return true;

    if (el.closest) {
      var header = el.closest('.file-header, .file-info, .repo-file-line');
      if (header) {
        var p = header.getAttribute('data-path') || header.getAttribute('data-file');
        if (hasDcExtension(p))
          return true;

        var link = header.querySelector('a[href]');
        if (link && hasDcExtension(link.getAttribute('href')))
          return true;
      }
    }
    return false;
  }

  function hasServerMarkup(el) {
    if (!el || !el.childNodes)
      return true;

    for (var i = 0; i < el.childNodes.length; i++)
      if (el.childNodes[i].nodeType === 1)
        return true;

    return false;
  }

  function collectCandidates() {
    var found = [];
    var seen = new WeakSet();

    function push(el) {
      if (el && !seen.has(el)) {
        seen.add(el);
        found.push(el);
      }
    }

    var codeNodes = document.querySelectorAll('pre code, td.lines-code code, .lines-code code, code.code-inner');
    for (var i = 0; i < codeNodes.length; i++)
      push(codeNodes[i]);

    var pres = document.querySelectorAll('pre.code-block, pre.code, .view-raw pre');
    for (var j = 0; j < pres.length; j++)
      if (!pres[j].querySelector('code'))
        push(pres[j]);

    var diffs = document.querySelectorAll('.diff-file-box td.lines-code span.code-inner, td.lines-code span.code-inner');
    for (var k = 0; k < diffs.length; k++)
      push(diffs[k]);

    return found;
  }

  function highlightBlock(el) {
    if (!window.Prism || !window.Prism.languages || !window.Prism.languages.dc)
      return false;

    try {
      if (!hasLanguageClass(el))
        el.classList.add(HIGHLIGHT_CLASS);

      window.Prism.highlightElement(el);
      el.setAttribute(MARKER_ATTR, SCRIPT_VERSION);
      return true;
    } catch (e) {
      if (window.console && window.console.warn)
        window.console.warn('[dc-highlight] failed on block:', e);

      return false;
    }
  }

  function highlightAllMatchingBlocks() {
    if (!window.Prism || !window.Prism.languages || !window.Prism.languages.dc)
      return 0;

    var blocks = collectCandidates();
    var count = 0;
    for (var i = 0; i < blocks.length; i++) {
      var el = blocks[i];
      if (!isDcBlock(el))
        continue;

      if (hasServerMarkup(el)) {
        el.setAttribute(MARKER_ATTR, 'skipped-server-markup');
        continue;
      }

      if (!el.textContent || !el.textContent.replace(/\s+/g, ''))
        continue;

      if (highlightBlock(el))
        count++;

    }

    return count;
  }

  var scheduled = false;
  function schedule() {
    if (scheduled)
      return;

    scheduled = true;
    window.setTimeout(function () {
      scheduled = false;
      highlightAllMatchingBlocks();
    }, DEBOUNCE_MS);
  }

  function observe() {
    if (!('MutationObserver' in window) || !document.documentElement)
      return;

    var observer = new MutationObserver(function (mutations) {
      for (var i = 0; i < mutations.length; i++) {
        var m = mutations[i];
        if (m.addedNodes && m.addedNodes.length > 0) {
          schedule();
          return;
        }

        if (m.type === 'attributes' && m.target) {
          var t = m.target;
          if (t.classList && (t.classList.contains('lines-code') || hasLanguageClass(t))) {
            schedule();
            return;
          }
        }
      }
    });

    observer.observe(document.documentElement, {
      childList: true,
      subtree: true,
      attributes: true,
      attributeFilter: ['class', 'data-path']
    });
  }

  function watchNavigation() {
    document.addEventListener('htmx:afterSwap', schedule, false);
    document.addEventListener('htmx:load', schedule, false);
    document.addEventListener('pjax:complete', schedule, false);
    document.addEventListener('pjax:success', schedule, false);

    try {
      var push = window.history.pushState;
      window.history.pushState = function () {
        var result = push.apply(this, arguments);
        schedule();
        return result;
      };

      var replace = window.history.replaceState;
      window.history.replaceState = function () {
        var result = replace.apply(this, arguments);
        schedule();
        return result;
      };

    } catch (e) { }

    window.addEventListener('popstate', schedule, false);
  }

  function boot() {
    var waited = 0;
    function ready() {
      if (window.Prism && window.Prism.languages && window.Prism.languages.dc) {
        highlightAllMatchingBlocks();
        observe();
        watchNavigation();
        return;
      }

      waited += PRISM_WAIT_MS;
      if (waited >= PRISM_WAIT_MAX_MS) {
        if (window.console && window.console.warn)
          window.console.warn('[dc-highlight] Prism unavailable, giving up.');

        return;
      }

      window.setTimeout(ready, PRISM_WAIT_MS);
    }

    ready();
  }

  if (document.readyState === 'loading')
    document.addEventListener('DOMContentLoaded', boot, false);
  else
    boot();

  window.addEventListener('load', schedule, false);

  window.DcGiteaHighlighter = {
    version: SCRIPT_VERSION,
    highlightAllMatchingBlocks: highlightAllMatchingBlocks
  };
})();
