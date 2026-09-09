import { mount } from 'svelte';
import App from './App.svelte';
import './style.css';
import './workflow.css';
import './pageFlow.css';
import { prefersStaticSite } from './motionPreference';

document.documentElement.dataset.motion = prefersStaticSite() ? 'reduced' : 'full';
mount(App, { target: document.getElementById('app')! });
