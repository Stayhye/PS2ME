// Subtle background: the four PS2 face-button symbols (cross, circle, triangle,
// square) drifting down and rotating in classic PlayStation colours, at low
// opacity. Dependency-free. Under prefers-reduced-motion the symbols still
// render but sit still (the CSS drops the animation).
(function () {
  'use strict';

  var SHAPES = {
    cross:    '<path d="M28 28 72 72M72 28 28 72"/>',
    circle:   '<circle cx="50" cy="50" r="26"/>',
    triangle: '<path d="M50 22 76 73 24 73Z"/>',
    square:   '<rect x="26" y="26" width="48" height="48" rx="5"/>',
  };
  var COLORS = {
    cross:    '#6ea8ff',   // blue
    circle:   '#ff6b8a',   // red / pink
    triangle: '#4be3a5',   // green
    square:   '#e79ad4',   // magenta
  };
  var TYPES = ['cross', 'circle', 'triangle', 'square'];
  var COUNT = 34;

  function rnd(min, max) { return min + Math.random() * (max - min); }

  var container = document.createElement('div');
  container.className = 'bg-symbols';
  container.setAttribute('aria-hidden', 'true');

  for (var i = 0; i < COUNT; i++) {
    var type = TYPES[i % TYPES.length];
    var size = rnd(18, 84);                       // wider size spread
    var span = document.createElement('span');
    span.className = 'bg-symbol';
    span.style.left = rnd(0, 97).toFixed(2) + '%';
    span.style.width = size.toFixed(0) + 'px';
    span.style.height = size.toFixed(0) + 'px';
    span.style.opacity = rnd(0.07, 0.26).toFixed(3);   // per-symbol alpha variation
    span.style.setProperty('--dur', rnd(16, 34).toFixed(1) + 's');
    span.style.setProperty('--delay', (-rnd(0, 34)).toFixed(1) + 's');   // stagger the loop
    var spin = rnd(200, 680) * (Math.random() < 0.5 ? -1 : 1);
    span.style.setProperty('--spin', spin.toFixed(0) + 'deg');
    span.style.color = COLORS[type];   // drives both stroke and the glow
    span.innerHTML =
      '<svg viewBox="0 0 100 100" fill="none" stroke="currentColor"' +
      ' stroke-width="8" stroke-linecap="round" stroke-linejoin="round">' +
      SHAPES[type] + '</svg>';
    container.appendChild(span);
  }

  document.body.insertBefore(container, document.body.firstChild);
})();
