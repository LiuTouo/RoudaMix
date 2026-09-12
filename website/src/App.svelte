<script lang="ts">
  import { onMount } from 'svelte';
  import gsap from 'gsap';
  import { ScrollTrigger } from 'gsap/ScrollTrigger';
  import Lenis from 'lenis';
  import 'lenis/dist/lenis.css';
  import Scene from './ScenePlayback.svelte';
  import AudioDemo from './AudioDemo.svelte';
  import { content, type Locale } from './content';
  import { chapters, chapterProgress, storyState, motion } from './story';
  import { publication } from './config';
  import { fetchLatestRelease, type LatestRelease } from './release';
  import { installPanelStack } from './panelStack';

  const locale: Locale = /\/en\/?$/.test(location.pathname) ? 'en' : 'zh';
  const t = content[locale];
  let progress = $state(0);
  let pageProgress = $state(0);
  let cursorX = $state(-100);
  let cursorY = $state(-100);
  let cursorActive = $state(false);
  let cursorVisible = $state(false);
  let currentSection = $state('top');
  let chapterCovered = $state(false);
  let storyCovered = $state(false);
  let storyElement: HTMLElement;
  let cursorElement: HTMLDivElement;
  let lenis: Lenis | undefined;
  let trigger: ScrollTrigger | undefined;
  let navigationTween: gsap.core.Tween | undefined;
  let navigationDestination: string | null = null;
  let navigating = $state(false);
  let copyElement = $state<HTMLDivElement>();
  let lastCopyStage = -1;
  let navigationVersion = 0;
  let latestNavigation = 'top';
  let latest = $state<LatestRelease | null>(null);
  const sceneState = $derived(storyState(progress));
  const chapterIndex = $derived(sceneState.stage);
  let demoPhase = $state(0);
  const languageHash = $derived(currentSection === 'story' ? chapters[sceneState.stage] : currentSection);

  $effect(() => {
    const stage = chapterIndex;
    const node = copyElement;
    if (!node) return;
    const direction = stage >= lastCopyStage ? 1 : -1;
    lastCopyStage = stage;
    const animations = Array.from(node.children).filter(element => !element.classList.contains('sr-only')).map((element, index) => element.animate([
      { opacity: 0, transform: `translateY(${direction * 64}px)`, filter: 'blur(4px)' },
      { opacity: 1, transform: 'translateY(0)', filter: 'blur(0px)' },
    ], { duration: 820, delay: index * 45, easing: 'cubic-bezier(.22,1,.36,1)', fill: 'backwards' }));
    return () => animations.forEach(animation => animation.cancel());
  });

  function navigateTo(id: string, updateHistory = true) {
    const index = chapters.indexOf(id as typeof chapters[number]);
    const element = document.getElementById(index >= 0 ? 'story' : id);
    if (!element) return;
    ++navigationVersion;
    const requestVersion = navigationVersion;
    latestNavigation = id;
    const anchor = document.getElementById(`${id}-start`) ?? element;
    let target = anchor.getBoundingClientRect().top + window.scrollY - 84;
    if (index >= 0 && trigger) target = trigger.start + chapterProgress(index) * (trigger.end - trigger.start);
    if (id === 'top') target = 0;
    navigationTween?.kill();
    navigationDestination = updateHistory ? id : null;
    navigating = navigationDestination !== null;
    window.dispatchEvent(new Event('roudamix:navigate'));
    if (updateHistory) history.pushState(null, '', `#${id}`);
    const focus = index >= 0 ? document.getElementById('story-heading') : element;
    const move = (position: number) => {
      if (lenis) lenis.scrollTo(position, { immediate: true });
      else window.scrollTo({ top: position, behavior: 'instant' });
      ScrollTrigger.update();
    };
    const arrive = () => {
      navigationDestination = null;
      window.dispatchEvent(new Event('roudamix:chapter-ready'));
      // The visible audio controls are only usable after the page and text settle.
      const entrances = copyElement?.getAnimations({ subtree: true }) ?? [];
      void Promise.allSettled(entrances.map(animation => animation.finished)).then(() => {
        if (requestVersion !== navigationVersion) return;
        navigating = false;
        focus?.focus({ preventScroll: true });
      });
    };
    if (updateHistory) {
      const scroll = { position: window.scrollY };
      navigationTween = gsap.to(scroll, { position: target, duration: Math.min(1.25, 0.65 + Math.abs(target - scroll.position) / 14000), ease: 'power3.inOut', onUpdate: () => move(scroll.position), onComplete: arrive });
    } else { move(target); arrive(); }
  }

  onMount(() => {
    fetchLatestRelease().then(release => { latest = release; });
    gsap.registerPlugin(ScrollTrigger);
    const media = gsap.matchMedia();
    const interruptScroll = () => { navigationTween?.kill(); navigationDestination = null; navigating = false; };
    window.addEventListener('wheel', interruptScroll, { passive: true });
    window.addEventListener('touchstart', interruptScroll, { passive: true });
    window.addEventListener('keydown', interruptScroll);
    const reveals: Animation[] = [];
    const revealObserver = new IntersectionObserver(entries => {
      for (const entry of entries) {
        if (!entry.isIntersecting) continue;
        reveals.push(entry.target.animate([
          { opacity: 0, transform: 'translateY(35px)' }, { opacity: 1, transform: 'translateY(0)' },
        ], { duration: 750, easing: 'cubic-bezier(.22,1,.36,1)' }));
        revealObserver.unobserve(entry.target);
      }
    }, { threshold: 0.08 });
    document.querySelectorAll('.section-heading,.guide-steps article,.download h2,.download .lead,.download-actions,.faq details').forEach(element => revealObserver.observe(element));
    const disposeStack = installPanelStack(Array.from(document.querySelectorAll<HTMLElement>('.stack-surface')));
    media.add({ all: '(min-width: 0px)', desktop: '(hover: hover) and (pointer: fine)' }, (context) => {
      if (context.conditions?.desktop) {
        lenis = new Lenis({ duration: motion.smoothingSeconds, easing: t => 1 - Math.pow(1 - t, 3), smoothWheel: true, syncTouch: false });
        lenis.on('scroll', ScrollTrigger.update);
        gsap.ticker.lagSmoothing(0);
      }
      const frame = (time: number) => { lenis?.raf(time * 1000); };
      gsap.ticker.add(frame);
      // CSS owns sticky layout; ScrollTrigger only reads a deterministic 0–1 clock.
      // Numeric bounds stay tied to normal flow even while the story panel is pinned.
      trigger = ScrollTrigger.create({ trigger: storyElement, start: () => 0, end: () => Math.max(1, storyElement.offsetHeight - innerHeight), onUpdate: self => { progress = self.progress; } });
      return () => { gsap.ticker.remove(frame); lenis?.destroy(); lenis = undefined; trigger?.kill(); };
    });
    const updatePage = () => {
      pageProgress = Math.max(0, Math.min(1, window.scrollY / Math.max(1, document.documentElement.scrollHeight - innerHeight)));
      const guide = document.getElementById('guide')!.getBoundingClientRect().top;
      const download = document.getElementById('download')!.getBoundingClientRect().top;
      chapterCovered = guide < innerHeight;
      storyCovered = guide <= 88;
      currentSection = download < innerHeight * 0.5 ? 'download' : guide < innerHeight * 0.5 ? 'guide' : window.scrollY > 60 ? 'story' : 'top';
    };
    const pointer = (event: PointerEvent) => {
      cursorX = event.clientX; cursorY = event.clientY;
      cursorVisible = event.pointerType === 'mouse';
      cursorActive = !!(event.target as HTMLElement).closest('a,button,summary');
    };
    const leave = () => { cursorVisible = false; };
    const hash = () => { if (location.hash) navigateTo(decodeURIComponent(location.hash.slice(1)), false); };
    let resizeTimer = 0;
    let resizeChapter: number | null = null;
    let resizingStory = false;
    let resizeNavigationVersion = 0;
    const rememberOrientation = () => {
      if (resizeChapter === null) {
        resizeChapter = sceneState.stage;
        resizingStory = currentSection === 'story';
        // An in-flight page turn must finish at its destination after resize.
        resizeNavigationVersion = navigationDestination !== null ? navigationVersion - 1 : navigationVersion;
      }
    };
    const resize = () => {
      rememberOrientation();
      clearTimeout(resizeTimer);
      // Run after ScrollTrigger's 200 ms resize refresh and orientation reversion.
      resizeTimer = window.setTimeout(() => {
        ScrollTrigger.refresh();
        if (navigationVersion !== resizeNavigationVersion) navigateTo(latestNavigation, false);
        else if (resizingStory && resizeChapter !== null) navigateTo(chapters[resizeChapter], false);
        resizeChapter = null;
        updatePage();
      }, 250);
    };
    window.addEventListener('scroll', updatePage, { passive: true });
    window.addEventListener('resize', resize);
    window.addEventListener('orientationchange', rememberOrientation);
    window.addEventListener('pointermove', pointer, { passive: true });
    document.documentElement.addEventListener('pointerleave', leave);
    window.addEventListener('hashchange', hash);
    const ready = requestAnimationFrame(() => { ScrollTrigger.refresh(); hash(); updatePage(); });
    return () => {
      cancelAnimationFrame(ready); clearTimeout(resizeTimer); media.revert();
      navigationTween?.kill();
      revealObserver.disconnect(); reveals.forEach(animation => animation.cancel());
      disposeStack();
      window.removeEventListener('wheel', interruptScroll); window.removeEventListener('touchstart', interruptScroll); window.removeEventListener('keydown', interruptScroll);
      window.removeEventListener('orientationchange', rememberOrientation);
      window.removeEventListener('scroll', updatePage); window.removeEventListener('resize', resize); window.removeEventListener('pointermove', pointer); document.documentElement.removeEventListener('pointerleave', leave); window.removeEventListener('hashchange', hash);
    };
  });
</script>

<a class="skip-link" href="#guide" onclick={(event) => { event.preventDefault(); navigateTo('guide'); }}>{t.skip}</a>
<header class="site-header">
  <a class="brand" href="#top" onclick={(event) => { event.preventDefault(); navigateTo('top'); }}><img src={`${import.meta.env.BASE_URL}brand.png`} alt="" /><span>RoudaMix<span class="brand-period">.</span></span></a>
  <nav aria-label={locale === 'zh' ? '主要導覽' : 'Main navigation'}>
    <a class="explore-link" href="#open" onclick={(event) => { event.preventDefault(); navigateTo('open'); }}>{t.nav[0]}</a>
    <a href="#guide" onclick={(event) => { event.preventDefault(); navigateTo('guide'); }}>{t.nav[1]}</a>
    <a class="language" href={`${import.meta.env.BASE_URL}${locale === 'zh' ? 'en/' : ''}#${languageHash}`} lang={locale === 'zh' ? 'en' : 'zh-Hant'}>{t.language}</a>
    <a class="nav-download" href="#download" onclick={(event) => { event.preventDefault(); navigateTo('download'); }}>{t.nav[2]} <span aria-hidden="true">↗</span></a>
  </nav>
</header>

<main id="top" tabindex="-1">
  <section class="story stack-surface" id="story" bind:this={storyElement} style={`--scroll-vh:${motion.scrollViewports * 100}svh`} aria-label={t.chapterLabel}>
    <div class="story-sticky">
      <div class="ambient-grid" aria-hidden="true"></div>
      <div class="story-layout">
        <div class="story-copy" bind:this={copyElement} data-chapter={chapters[sceneState.stage]}>
          {#if sceneState.stage === 0}
            <p class="eyebrow"><span class="live-dot"></span>{t.eyebrow}</p>
            <h1>{t.hero[0]}<br /><span>{t.hero[1]}</span></h1>
            <p class="lead">{t.intro}</p>
            <div class="hero-actions"><a class="button primary" href="#download" onclick={(event) => { event.preventDefault(); navigateTo('download'); }}>{t.cta}<span aria-hidden="true">↗</span></a></div>
            <p class="platform mono">WINDOWS x64 <span>/</span> VST3 HOST <span>/</span> YOUR MIX</p>
          {:else}
            <p class="eyebrow"><span class="chapter-number mono">0{sceneState.stage + 1} / 07</span>{t.scenes[sceneState.stage][2]}</p>
            <h2 class="scene-heading">{t.scenes[sceneState.stage][0]}</h2>
            <p class="lead">{t.scenes[sceneState.stage][1]}</p>
            {#if sceneState.stage === 4}<AudioDemo {locale} active={sceneState.stage === 4} suspended={navigating} />{/if}
            {#if sceneState.stage === 6}<a class="text-link" href="#guide" onclick={(event) => { event.preventDefault(); navigateTo('guide'); }}>{t.nav[1]} <span aria-hidden="true">↗</span></a>{/if}
          {/if}
          <span class="sr-only" id="story-heading" tabindex="-1">{t.scenes[sceneState.stage][0]}</span>
        </div>
        <Scene {progress} {locale} covered={storyCovered} onphase={(p) => demoPhase = p} />
      </div>
      <div class="story-bottom"><span class="scroll-instruction"><span aria-hidden="true">↓</span>{t.scroll}</span><a href="#guide" onclick={(event) => { event.preventDefault(); navigateTo('guide'); }}>{t.skip} <span aria-hidden="true">↘</span></a></div>
    </div>
  </section>

  <nav class="chapter-nav" class:out-of-story={chapterCovered} aria-label={t.chapterLabel}>
    {#each chapters as id, index}<a href={`#${id}`} aria-label={`${index + 1}. ${t.scenes[index][2]}`} aria-current={sceneState.stage === index ? 'step' : undefined} onclick={(event) => { event.preventDefault(); navigateTo(id); }}><span class="mono">0{index + 1}</span><span class="chapter-name">{t.scenes[index][2]}</span><i style={`--fill:${sceneState.stage > index ? 1 : sceneState.stage === index ? demoPhase : 0}`}></i></a>{/each}
  </nav>

  <span id="guide-start" class="section-anchor" aria-hidden="true"></span>
  <section class="guide section-wrap stack-surface cover-panel" id="guide" tabindex="-1">
    <div class="section-heading"><p class="eyebrow">{t.guideLabel}</p><h2>{t.guideTitle}</h2><p class="lead">{t.guideIntro}</p></div>
    <div class="guide-steps">{#each t.steps as step, index}<article><span class="step-number mono">0{index + 1}</span><div><h3>{step[0]}</h3><p>{step[1]}</p>{#if index === 4}<a class="text-link" href="https://vb-audio.com/Cable/">VB-Audio / VB-CABLE ↗</a>{/if}</div></article>{/each}</div>
  </section>

  <span id="download-start" class="section-anchor" aria-hidden="true"></span>
  <section class="download section-wrap stack-surface cover-panel" id="download" tabindex="-1">
    <div class="download-top"><span class="eyebrow">MAKE IT YOUR MIX</span><span class="mono">ROUDAMIX / WINDOWS</span></div>
    <h2>{t.downloadTitle}</h2><p class="lead">{t.downloadBody}</p>
    <div class="download-actions">{#if latest}<a class="button primary" href={latest.setupUrl}>{t.nav[2]} v{latest.version} ↗</a><a class="text-link" href={latest.portableUrl}>{t.portable} ↗</a>{/if}<a class="text-link" href={publication.repository}>{t.github} ↗</a></div>
    <p class="requirements">{t.requirements}</p>
  </section>

  <span id="faq-start" class="section-anchor" aria-hidden="true"></span>
  <section class="faq section-wrap stack-surface cover-panel" id="faq"><p class="eyebrow">GOOD TO KNOW / FAQ</p><h2>{t.faqTitle}</h2><div>{#each t.faq as item}<details><summary>{item[0]}<span aria-hidden="true">＋</span></summary><p>{item[1]}</p></details>{/each}</div></section>
</main>

<footer class="section-wrap"><div><a class="brand" href="#top" onclick={(event) => { event.preventDefault(); navigateTo('top'); }}><img src={`${import.meta.env.BASE_URL}brand.png`} alt="" />RoudaMix.</a><p>{t.footer}</p></div><span class="mono">GPL-3.0-only · 2026</span><a href="#top" onclick={(event) => { event.preventDefault(); navigateTo('top'); }}>{t.back} ↑</a></footer>
<div class="cursor" bind:this={cursorElement} class:active={cursorActive} class:visible={cursorVisible} aria-hidden="true" style={`transform:translate3d(${cursorX}px,${cursorY}px,0); --page-progress:${pageProgress * 360}deg`}><span></span></div>
