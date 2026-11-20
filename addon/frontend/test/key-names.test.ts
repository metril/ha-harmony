import { describe, expect, it } from 'vitest';
import { displayKey } from '../src/lib/key-names';
import { keyLabel } from '../src/lib/bt-model';
import { keyTitle } from '../src/lib/remote-layout';

describe('displayKey', () => {
  it.each([
    ['home_control_1', 'Upper bulb'], ['home_control_2', 'Lower bulb'], ['home_control_3', 'Upper socket'], ['home_control_4', 'Lower socket'],
    ['prev_channel', 'Previous channel'], ['PrevChannel', 'Previous channel'], ['channel_up', 'Channel up'], ['ChannelUp', 'Channel up'],
    ['volume_down', 'Volume down'], ['VolumeDown', 'Volume down'], ['select', 'OK'], ['Menu', 'Menu'], ['dvr', 'DVR'], ['DVR', 'DVR'],
    ['fast_forward', 'Fast forward'], ['FastForward', 'Fast forward'], ['Num5', '5'], ['key_105', '5'], ['key_7', 'Key 7'],
    ['hdmi_input_2', 'HDMI input 2'], ['direction_up', 'Direction up'], ['DirectionUp', 'Direction up'], ['red', 'Red'],
  ])('%s -> %s', (raw, want) => expect(displayKey(raw)).toBe(want));
  it('never leaves underscores or a lower-case start', () => {
    for (const k of ['some_new_key', 'SomeNewKey', 'x_1', 'a']) {
      const t = displayKey(k);
      expect(t).not.toContain('_');
      expect(t[0]).toBe(t[0]?.toUpperCase());
    }
  });
  it('is what the remote and the Bluetooth views show', () => {
    expect(keyTitle('prev_channel')).toContain('Previous channel');
    expect(keyLabel('volume_up')).toBe('Volume up');
    expect(keyLabel('key_103')).toBe('Digit 3');
  });
});
