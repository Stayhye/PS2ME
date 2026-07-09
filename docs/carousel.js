// Minimal, dependency-free carousel: native scroll-snap for swiping, with prev/next
// buttons and clickable dots. Navigating scrolls only the track (block: 'nearest'),
// so the page never jumps vertically.
(function () {
  'use strict';

  function initCarousel(car) {
    var track = car.querySelector('[data-car-track]');
    if (!track) return;
    var slides = Array.prototype.slice.call(track.children);
    var prev = car.querySelector('[data-car-prev]');
    var next = car.querySelector('[data-car-next]');
    var dotsWrap = car.querySelector('[data-car-dots]');

    function goTo(slide) {
      slide.scrollIntoView({ behavior: 'smooth', inline: 'center', block: 'nearest' });
    }

    var dots = slides.map(function (slide, i) {
      var dot = document.createElement('button');
      dot.className = 'car-dot';
      dot.type = 'button';
      dot.setAttribute('aria-label', 'Go to screenshot ' + (i + 1));
      dot.addEventListener('click', function () { goTo(slide); });
      if (dotsWrap) dotsWrap.appendChild(dot);
      return dot;
    });

    function currentIndex() {
      var center = track.scrollLeft + track.clientWidth / 2;
      var best = 0, bestDist = Infinity;
      for (var i = 0; i < slides.length; i++) {
        var slideCenter = slides[i].offsetLeft + slides[i].offsetWidth / 2;
        var dist = Math.abs(slideCenter - center);
        if (dist < bestDist) { bestDist = dist; best = i; }
      }
      return best;
    }

    function refresh() {
      var idx = currentIndex();
      for (var i = 0; i < dots.length; i++) {
        dots[i].classList.toggle('is-active', i === idx);
      }
    }

    function step(delta) {
      var idx = Math.max(0, Math.min(slides.length - 1, currentIndex() + delta));
      goTo(slides[idx]);
    }

    if (prev) prev.addEventListener('click', function () { step(-1); });
    if (next) next.addEventListener('click', function () { step(1); });
    track.addEventListener('scroll', function () { window.requestAnimationFrame(refresh); });
    window.addEventListener('resize', refresh);
    refresh();
  }

  document.querySelectorAll('[data-carousel]').forEach(initCarousel);
})();
