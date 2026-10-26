// @vitest-environment jsdom
import { afterEach, expect, it } from 'vitest';
import { flushSync, mount, unmount } from 'svelte';
import ContainerPrototype from '../../ContainerPrototype.svelte';

let component: ReturnType<typeof mount>;
afterEach(async () => {
  if (component) await unmount(component);
  document.body.innerHTML = '';
  history.replaceState(null, '', '/');
});

function render(variant: string) {
  history.replaceState(null, '', `/?prototype=containers&variant=${variant}`);
  component = mount(ContainerPrototype, { target: document.body });
  flushSync();
}
function click(label: string) {
  document.querySelector<HTMLButtonElement>(`[aria-label="${label}"]`)!.click();
  flushSync();
}

it.each(['A', 'B', 'C'])('ContainerPrototype deep link %s has distinct layout and two populated containers', variant => {
  render(variant);
  expect(document.querySelector(`[data-variant="${variant}"]`)).not.toBeNull();
  expect(document.querySelectorAll('.container-well')).toHaveLength(2);
  expect(document.querySelectorAll('.container-well:not(.output-well) .study-channel')).toHaveLength(4);
  expect(document.querySelectorAll('.output-well .study-channel')).toHaveLength(2);
  expect(document.querySelector('.state-panel pre')!.textContent).toContain(`"variant": "${variant}"`);
  expect(document.body.textContent).toContain('MOCK · 不連接音訊');
});

it('ContainerPrototype wraps, normalizes invalid URLs, respects focused controls and responds to history', () => {
  render('invalid');
  expect(location.search).toContain('variant=A');
  click('上一個方案');
  expect(location.search).toContain('variant=C');
  window.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowRight' }));
  flushSync();
  expect(location.search).toContain('variant=A');
  for (const selector of ['input', 'button', '.channel-lane']) {
    document.querySelector(selector)!.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowLeft', bubbles: true }));
    flushSync();
    expect(location.search).toContain('variant=A');
  }
  window.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowRight', ctrlKey: true }));
  flushSync();
  expect(location.search).toContain('variant=A');
  history.replaceState(null, '', '/?prototype=containers&variant=B');
  window.dispatchEvent(new PopStateEvent('popstate'));
  flushSync();
  expect(document.querySelector('[data-variant="B"]')).not.toBeNull();
});

it('ContainerPrototype keeps gain, mute, bypass and routing local across variants and resets them', () => {
  render('A');
  const gain = document.querySelector<HTMLInputElement>('#gain-mic')!;
  gain.value = '-9.5';
  gain.dispatchEvent(new Event('input', { bubbles: true }));
  click('人聲 靜音（模擬）');
  click('人聲 效果器旁通（模擬）');
  click('人聲 傳送至監聽（模擬）');
  click('下一個方案');
  const state = () => JSON.parse(document.querySelector('.state-panel pre')!.textContent!);
  expect(state().tracks[0]).toMatchObject({ gain: -9.5, mute: true, bypass: true, monitor: false });
  expect(document.querySelector('#gain-mic')!.getAttribute('aria-valuetext')).toBe('-9.5 dB');
  expect(document.querySelector('[aria-label="人聲 示意電平 靜音"]')).not.toBeNull();
  document.querySelector<HTMLButtonElement>('.reset')!.click();
  flushSync();
  expect(state().tracks[0]).toMatchObject({ gain: -3, mute: false, bypass: false, monitor: true });
  expect(state().variant).toBe('B');
});
