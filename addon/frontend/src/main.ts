import './app.css';
import { Api } from './lib/api';
import { pickConfig, pickHub, rememberedHub } from './lib/hubs';
import { Store, setStore } from './lib/store';
import './components/hh-app';

async function boot(): Promise<void> {
  const api = new Api();
  // ?hub= wins; else the hub id Home Assistant is linked for; never `default` once a linked hub exists.
  let hub = 'default';
  let hubs: Awaited<ReturnType<Api['hubs']>> | null = null;
  try {
    hubs = await api.hubs();
    hub = pickHub(location.search, hubs, rememberedHub()).hub;
  } catch {
    hub = pickHub(location.search, null).hub;
  }
  // the library config: ?config=, else the one the hub runs, else the newest; an empty library gets "Main"
  let configId = '';
  try {
    const lib = (await api.configs()).configs;
    configId = pickConfig(location.search, lib, hubs, hub) ?? (await api.createConfig({ name: 'Main' })).id;
  } catch {
    configId = 'main';
  }
  const store = new Store(api, hub, configId);
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
