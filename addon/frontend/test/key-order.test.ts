import { describe, expect, it } from 'vitest';
import { canonicalKey, orderBy, orderUnits } from '../src/lib/key-order';

const names = (l: readonly string[]) => orderBy(l, (x) => x);
describe('canonical key order', () => {
  it('normalises spellings from IR, Home Assistant and Bluetooth', () => {
    expect(['volume_up', 'vol_up', 'VolumeUp', 'Volume Up'].map(canonicalKey)).toEqual(Array(4).fill('volume_up'));
    expect(['FastForward', 'ffwd', 'fast_forward'].map(canonicalKey)).toEqual(Array(3).fill('fast_forward'));
    expect(['DirectionUp', 'dpad_up', 'up'].map(canonicalKey)).toEqual(Array(3).fill('up'));
    expect(['num_5', 'Digit5', 'key_105', '5'].map(canonicalKey)).toEqual(Array(4).fill('5'));
  });
  it('orders navigation, volume, transport, colours, numbers, power and home control', () => {
    expect(names(['info', 'guide', 'menu', 'back', 'ok', 'right', 'left', 'down', 'up'])).toEqual(['up', 'down', 'left', 'right', 'ok', 'back', 'menu', 'guide', 'info']);
    expect(names(['mute', 'volume_down', 'volume_up'])).toEqual(['volume_up', 'volume_down', 'mute']);
    expect(names(['record', 'stop', 'fast_forward', 'pause', 'rewind', 'play', 'skip_forward', 'skip_back'])).toEqual(['play', 'pause', 'rewind', 'fast_forward', 'skip_back', 'skip_forward', 'stop', 'record']);
    expect(names(['blue', 'yellow', 'green', 'red'])).toEqual(['red', 'green', 'yellow', 'blue']);
    expect(names(['0', '9', '1', '5'])).toEqual(['1', '5', '9', '0']);
    expect(names(['power_off', 'power_toggle', 'power_on'])).toEqual(['power_on', 'power_off', 'power_toggle']);
    expect(names(['home_control_minus', 'home_control_3', 'home_control_plus', 'home_control_1', 'home_control_4', 'home_control_2']))
      .toEqual(['home_control_1', 'home_control_2', 'home_control_3', 'home_control_4', 'home_control_plus', 'home_control_minus']);
  });
  it('puts unknown names after the known ones, in natural order', () => {
    expect(names(['hdmi10', 'zoom', 'hdmi2', 'ok', 'hdmi1'])).toEqual(['ok', 'hdmi1', 'hdmi2', 'hdmi10', 'zoom']);
  });
  it('groups left|right pairs into one unit, left then right', () => {
    const u = orderUnits(['fast_forward', 'mute', 'play', 'rewind', 'pause', 'ch_down', 'ch_up'], (x) => x);
    expect(u.map((x) => x.join('|'))).toEqual(['mute', 'ch_up|ch_down', 'play|pause', 'rewind|fast_forward']);
  });
  it('puts play_pause on its own row after the Play|Pause pair', () => {
    expect(orderUnits(['stop', 'play_pause', 'pause', 'play'], (x) => x).map((x) => x.join('|'))).toEqual(['play|pause', 'play_pause', 'stop']);
  });
  it('does not pair a member with a stranger', () => {
    expect(orderUnits(['volume_up', 'mute'], (x) => x).map((x) => x.length)).toEqual([1, 1]);
    expect(orderUnits(['pause', 'stop'], (x) => x).map((x) => x.join('|'))).toEqual(['pause', 'stop']);
  });
});
