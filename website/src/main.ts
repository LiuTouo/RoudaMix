import { mount } from 'svelte';
import App from './App.svelte';
import './style.css';
import './workflow.css';
import './pageFlow.css';

// #app 內的靜態 SEO 內容(爬蟲用)在掛載前清掉;Svelte mount 是 append 不會清空
document.getElementById('app')!.textContent = '';
mount(App, { target: document.getElementById('app')! });
