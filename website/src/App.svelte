<script lang="ts">
  import { onMount } from 'svelte';
  import gsap from 'gsap';
  import { ScrollTrigger } from 'gsap/ScrollTrigger';
  import Lenis from 'lenis';
  import 'lenis/dist/lenis.css';
  import Scene from './Scene.svelte';
  import AudioDemo from './AudioDemo.svelte';
  import { content, type Locale } from './content';
  import { chapters, chapterProgress, storyState, motion } from './story';
  import { publication } from './config';

  const locale: Locale = /\/en\/?$/.test(location.pathname) ? 'en' : 'zh';
  const t = content[locale];
  let progress = $state(0);
  let pageProgress = $state(0);
  let reduced = $state(matchMedia('(prefers-reduced-motion: reduce)').matches);
  let cursorX = $state(-100);
  let cursorY = $state(-100);
  let cursorActive = $state(false);
  let cursorVisible = $state(false);
  let currentSection = $state('top');
  let storyElement: HTMLElement;
  let cursorElement: HTMLDivElement;
  let lenis: Lenis | undefined;
  let trigger: ScrollTrigger | undefined;
  let navigationVersion = 0;
  let latestNavigation = 'top';
  const sceneState = $derived(storyState(progress));
  const languageHash = $derived(currentSection === 'story' ? chapters[sceneState.stage] : currentSection);

  function navigateTo(id: string, updateHistory = true) {
    const index = chapters.indexOf(id as typeof chapters[number]);
    const element = document.getElementById(index >= 0 && !reduced ? 'story' : id);
    if (!element) return;
    ++navigationVersion;
    latestNavigation = id;
    let target = element.getBoundingClientRect().top + window.scrollY - 84;
    if (index >= 0 && !reduced && trigger) target = trigger.start + chapterProgress(index) * (trigger.end - trigger.start);
    if (id === 'top') target = 0;
    if (lenis) lenis.scrollTo(target, { immediate: true });
    else window.scrollTo({ top: target, behavior: 'instant' });
    ScrollTrigger.update();
    if (updateHistory) history.pushState(null, '', `#${id}`);
    const focus = index >= 0 && !reduced ? document.getElementById('story-heading') : element;
    focus?.focus({ preventScroll: true });
  }

  onMount(() => {
    gsap.registerPlugin(ScrollTrigger);
    const media = gsap.matchMedia();
    media.add({ all: '(min-width: 0px)', desktop: '(hover: hover) and (pointer: fine)', reduce: '(prefers-reduced-motion: reduce)' }, (context) => {
      reduced = !!context.conditions?.reduce;
      if (!reduced && context.conditions?.desktop) {
        lenis = new Lenis({ duration: motion.smoothingSeconds, smoothWheel: true, syncTouch: false });
        lenis.on('scroll', ScrollTrigger.update);
        gsap.ticker.lagSmoothing(0);
      }
      const frame = (time: number) => { lenis?.raf(time * 1000); };
      gsap.ticker.add(frame);
      // CSS owns sticky layout; ScrollTrigger only reads a deterministic 0–1 clock.
      trigger = ScrollTrigger.create({ trigger: storyElement, start: 'top top', end: 'bottom bottom', onUpdate: self => { progress = self.progress; } });
      return () => { gsap.ticker.remove(frame); lenis?.destroy(); lenis = undefined; trigger?.kill(); };
    });
    const updatePage = () => {
      pageProgress = Math.max(0, Math.min(1, window.scrollY / Math.max(1, document.documentElement.scrollHeight - innerHeight)));
      const guide = document.getElementById('guide')!.getBoundingClientRect().top;
      const download = document.getElementById('download')!.getBoundingClientRect().top;
      currentSection = download < innerHeight * 0.5 ? 'download' : guide < innerHeight * 0.5 ? 'guide' : window.scrollY > 60 ? 'story' : 'top';
      if (reduced && currentSection === 'story') {
        const active = chapters.findLastIndex(id => document.getElementById(id)!.getBoundingClientRect().top < innerHeight * 0.5);
        progress = chapterProgress(Math.max(0, active));
      }
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
        resizeNavigationVersion = navigationVersion;
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

<main id="top" tabindex="-1" class:reduced>
  <section class="story" id="story" bind:this={storyElement} style={`--scroll-vh:${motion.scrollViewports * 100}svh`} aria-label={t.chapterLabel}>
    {#if !reduced}
      <div class="story-sticky">
        <div class="ambient-grid" aria-hidden="true"></div>
        <div class="story-layout">
          <div class="story-copy" data-chapter={chapters[sceneState.stage]}>
            {#if sceneState.stage === 0}
              <p class="eyebrow"><span class="live-dot"></span>{t.eyebrow}</p>
              <h1>{t.hero[0]}<br /><span>{t.hero[1]}</span></h1>
              <p class="lead">{t.intro}</p>
              <div class="hero-actions"><a class="button primary" href="#download" onclick={(event) => { event.preventDefault(); navigateTo('download'); }}>{t.cta}<span aria-hidden="true">↗</span></a><span class="availability">{t.availability}</span></div>
              <p class="platform mono">WINDOWS x64 <span>/</span> VST3 HOST <span>/</span> YOUR MIX</p>
            {:else}
              <p class="eyebrow"><span class="chapter-number mono">0{sceneState.stage + 1} / 07</span>{t.scenes[sceneState.stage][2]}</p>
              <h2 class="scene-heading">{t.scenes[sceneState.stage][0]}</h2>
              <p class="lead">{t.scenes[sceneState.stage][1]}</p>
              {#if sceneState.stage === 4}<AudioDemo {locale} active={sceneState.stage === 4} />{/if}
              {#if sceneState.stage === 6}<a class="text-link" href="#guide" onclick={(event) => { event.preventDefault(); navigateTo('guide'); }}>{t.nav[1]} <span aria-hidden="true">↗</span></a>{/if}
            {/if}
            <span class="sr-only" id="story-heading" tabindex="-1">{t.scenes[sceneState.stage][0]}</span>
          </div>
          <Scene {progress} {locale} />
        </div>
        <div class="story-bottom"><span class="scroll-instruction"><span aria-hidden="true">↓</span>{t.scroll}</span><a href="#guide" onclick={(event) => { event.preventDefault(); navigateTo('guide'); }}>{t.skip} <span aria-hidden="true">↘</span></a></div>
      </div>
    {:else}
      <div class="static-intro"><p class="eyebrow">{t.eyebrow}</p><h1>{t.hero[0]}<br /><span>{t.hero[1]}</span></h1><p class="lead">{t.intro}</p><a class="button primary" href="#download">{t.cta} ↗</a></div>
      {#each t.scenes as scene, index}<article class="static-chapter" id={chapters[index]} tabindex="-1"><div><p class="eyebrow">0{index + 1} / {scene[2]}</p><h2>{scene[0]}</h2><p class="lead">{scene[1]}</p>{#if index === 4}<AudioDemo {locale} active={true} />{/if}</div><Scene progress={chapterProgress(index)} {locale} staticView /></article>{/each}
    {/if}
  </section>

  <nav class="chapter-nav" class:out-of-story={currentSection === 'guide' || currentSection === 'download'} aria-label={t.chapterLabel}>
    {#each chapters as id, index}<a href={`#${id}`} aria-label={`${index + 1}. ${t.scenes[index][2]}`} aria-current={sceneState.stage === index ? 'step' : undefined} onclick={(event) => { event.preventDefault(); navigateTo(id); }}><span class="mono">0{index + 1}</span><span class="chapter-name">{t.scenes[index][2]}</span><i style={`--fill:${sceneState.stage > index ? 1 : sceneState.stage === index ? sceneState.local : 0}`}></i></a>{/each}
  </nav>

  <section class="guide section-wrap" id="guide" tabindex="-1">
    <div class="section-heading"><p class="eyebrow">{t.guideLabel}</p><h2>{t.guideTitle}</h2><p class="lead">{t.guideIntro}</p></div>
    <div class="guide-steps">{#each t.steps as step, index}<article><span class="step-number mono">0{index + 1}</span><div><h3>{step[0]}</h3><p>{step[1]}</p>{#if index === 4}<a class="text-link" href="https://vb-audio.com/Cable/">VB-Audio / VB-CABLE ↗</a>{/if}</div></article>{/each}</div>
  </section>

  <section class="download section-wrap" id="download" tabindex="-1">
    <div class="download-top"><span class="eyebrow">MAKE IT YOUR MIX</span><span class="mono">ROUDAMIX / WINDOWS</span></div>
    <h2>{t.downloadTitle}</h2><p class="lead">{t.downloadBody}</p>
    <div class="download-actions">{#if publication.release}<a class="button primary" href={publication.release.url}>{t.nav[2]} v{publication.release.version} ↗</a>{:else}<p class="release-pending"><span class="live-dot"></span>{t.availability}</p>{/if}<a class="text-link" href={publication.repository}>{t.github} ↗</a></div>
    <p class="requirements">{t.requirements}</p>
  </section>

  <section class="faq section-wrap"><p class="eyebrow">GOOD TO KNOW / FAQ</p><h2>{t.faqTitle}</h2><div>{#each t.faq as item}<details><summary>{item[0]}<span aria-hidden="true">＋</span></summary><p>{item[1]}</p></details>{/each}</div></section>
</main>

<footer class="section-wrap"><div><a class="brand" href="#top" onclick={(event) => { event.preventDefault(); navigateTo('top'); }}><img src={`${import.meta.env.BASE_URL}brand.png`} alt="" />RoudaMix.</a><p>{t.footer}</p></div><span class="mono">GPL-3.0-only · 2026</span><a href="#top" onclick={(event) => { event.preventDefault(); navigateTo('top'); }}>{t.back} ↑</a></footer>
<div class="cursor" bind:this={cursorElement} class:active={cursorActive} class:visible={cursorVisible} aria-hidden="true" style={`transform:translate3d(${cursorX}px,${cursorY}px,0); --page-progress:${pageProgress * 360}deg`}><span></span></div>
