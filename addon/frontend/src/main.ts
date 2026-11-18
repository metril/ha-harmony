import './app.css';
import { Api } from './lib/api';
import { hubFromSearch } from './lib/router';
import { Store, setStore } from './lib/store';
import './components/hh-app';

async function boot(): Promise<void> {
  const api = new Api();
  let hub = hubFromSearch(location.search);
  if (!new URLSearchParams(location.search).has('hub')) {
    // No ?hub=: use the hub the add-on already holds (the integration's id) rather than an empty "default".
    try {
      const ids = (await api.hubs()).hubs.map((h) => h.id);
      if (ids.length && !ids.includes(hub)) hub = ids[0] as string;
    } catch {
      /* single-hub fallback */
    }
  }
  const store = new Store(api, hub);
  setStore(store);
  void store.load();
  // Elements read the store when they are created, so the app element is only made once it exists
  // (the bundle is one chunk, so the class definitions themselves run before boot()).
  document.body.prepend(document.createElement('hh-app'));
}
void boot();
