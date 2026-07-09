// Screenshots carousel, powered by Swiper (https://swiperjs.com — MIT).
// Loaded from a pinned CDN build; this file only wires up the instance.
(function () {
  'use strict';

  function init() {
    if (typeof Swiper === 'undefined') return;   // CDN blocked/offline
    new Swiper('.screenshots-swiper', {
      slidesPerView: 1,
      spaceBetween: 16,
      centeredSlides: true,
      grabCursor: true,
      loop: true,
      keyboard: { enabled: true },
      navigation: {
        prevEl: '.screenshots-swiper .swiper-button-prev',
        nextEl: '.screenshots-swiper .swiper-button-next',
      },
      pagination: {
        el: '.screenshots-swiper .swiper-pagination',
        clickable: true,
      },
      breakpoints: {
        // On wider screens, let the neighbouring slides peek in.
        760: { slidesPerView: 1.3 },
      },
    });
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }
})();
