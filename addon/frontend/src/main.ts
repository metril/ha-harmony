import './app.css';
import { Api } from './lib/api';
import { pickHub } from './lib/hubs';
import { Store, setStore } from './lib/store';
import './components/hh-app';

async function boot(): Promise<void> {
  const api = new Api();
  // ?hub= wins; else the hub id Home Assistant polls for; never `default` once a linked hub exists.
  let hub = 'default';
  try {
    hub = pickHub(location.search, await api.hubs()).hub;
  } catch {
    hub = pickHub(location.search, null).hub;
  }
  const store = new Store(api, hub);
  setStore(store);
  void store.load();
  addEventListener('pagehide', () => store.flushOnUnload());
  document.addEventListener('visibilitychange', () => {
    if (document.visibilityState === 'hidden') store.flushOnUnload();
  });
  // Elements read the store when they are created, so the app element is only made once it exists
  // (the bundle is one chunk, so the class definitions themselves run before boot()).
  document.body.prepend(document.createElement('hh-app'));
}
void boot();
