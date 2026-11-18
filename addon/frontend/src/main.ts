import './app.css';
import { Api } from './lib/api';
import { hubFromSearch } from './lib/router';
import { Store, setStore } from './lib/store';

const store = new Store(new Api(), hubFromSearch(location.search));
setStore(store);
void store.load();
// Elements read the store when they are created, so define them only after it exists.
void import('./components/hh-app');
