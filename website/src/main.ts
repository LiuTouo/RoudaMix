import { mount } from 'svelte';
import App from './App.svelte';
import './style.css';
import './workflow.css';
import './pageFlow.css';

mount(App, { target: document.getElementById('app')! });
