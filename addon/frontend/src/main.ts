import './app.css';
import { Api } from './lib/api';
import { hubFromSearch } from './lib/router';
import { Store, setStore } from './lib/store';
import './components/hh-app';

const store = new Store(new Api(), hubFromSearch(location.search));
setStore(store);
void store.load();
