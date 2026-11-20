import { beforeEach } from 'vitest';
import { FakeWS, fakeLive } from './fake-ws';

beforeEach(() => {
  fakeLive.reset();
  (globalThis as unknown as { WebSocket: unknown }).WebSocket = FakeWS;
});
