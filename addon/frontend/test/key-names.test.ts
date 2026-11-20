import { describe, expect, it } from 'vitest';
import { displayCommand, displayKey, gridStyle, spokenKey } from '../src/lib/key-names';
import { keyLabel } from '../src/lib/bt-model';
import { keyTitle } from '../src/lib/remote-layout';

describe('displayCommand', () => {
  it.each([['power_on', 'Power On'], ['vol_up', 'Volume Up'], ['mute', 'Mute'], ['hdmi1', 'HDMI 1'], ['ok', 'OK'], ['menu', 'Menu'], ['stream', 'Stream'], ['ch_up', 'Channel Up'],
    ['InputA1', 'Input A1'], ['input_a1', 'Input A1'], ['HDMI1', 'HDMI 1']])('%s -> %s', (a, b) => expect(displayCommand(a)).toBe(b));
  it('sizes a grid column to the longest label', () => expect(gridStyle(['a', 'Previous Channel'])).toContain('16ch'));
});

describe('displayKey: codes and acronyms', () => {
  it.each([
    ['InputA1', 'Input A1'], ['input_a1', 'Input A1'], ['A1', 'A1'], ['a1', 'A1'], ['F12', 'F12'], ['b2', 'B2'],
    ['1a', '1A'], ['1A', '1A'], ['4k', '4K'], ['3D', '3D'], ['5g', '5G'],
    ['HDMI1', 'HDMI 1'], ['hdmi1', 'HDMI 1'], ['hdmi_2', 'HDMI 2'], ['InputHdmi1', 'Input HDMI 1'], ['av1', 'AV 1'], ['usb', 'USB'], ['dvd_menu', 'DVD Menu'],
    ['eARC', 'eARC'], ['earc', 'eARC'], ['AM/FM', 'AM/FM'], ['tv_input', 'TV Input'], ['BT_pair', 'BT Pair'],
  ])('%s -> %s', (raw, want) => expect(displayKey(raw)).toBe(want));
  it('keeps units lower-case and symbols on the ends', () => {
    expect(displayKey('30s')).toBe('30s');
    expect(displayKey('Ch+')).toBe('Ch+');
    expect(displayKey('Vol-')).toBe('Vol-');
    expect(displayKey('Play/Pause')).toBe('Play/Pause');
  });
  it('shows a name that is already written for people as written', () => {
    for (const n of ['Watch TV', 'Living Room', 'Clock Set', 'Input B2', 'J-Tech AV Switch', 'NVIDIA SHIELD']) expect(displayKey(n)).toBe(n);
    expect(displayKey('watch tv')).toBe('Watch Tv');
    expect(displayKey('Shelf low')).toBe('Shelf Low');
    expect(displayKey('Power on')).toBe('Power On');
    expect(displayKey('App a')).toBe('App A');
    expect(displayKey('Tropical twilight')).toBe('Tropical Twilight');
    expect(displayKey('Back to tv')).toBe('Back to Tv');
    expect(displayKey('iPlayer app')).toBe('iPlayer App');
  });
  it('spells the special cases', () => {
    const want: [string, string][] = [['PlayPause', 'Play/Pause'], ['play_pause', 'Play/Pause'], ['arrowup', 'Arrow Up'], ['ArrowLeft', 'Arrow Left'], ['capslock', 'Caps Lock'],
      ['DPAD_CENTER', 'D-Pad Center'], ['brightness_pct', 'Brightness Percent'], ['necext', 'NECext'], ['nec', 'NEC'], ['rc5', 'RC5'], ['sirc12', 'SIRC 12'], ['ctrl+c', 'Ctrl+C']];
    for (const [raw, shown] of want) expect(displayKey(raw)).toBe(shown);
  });
  it('shows ids and codes as they are', () => {
    expect(displayKey('light.corner_lamp')).toBe('light.corner_lamp');
    expect(displayKey('HID:0C:0223')).toBe('HID:0C:0223');
  });
  it('turns ALL CAPS words into words but keeps real acronyms', () => {
    expect(displayKey('BACK')).toBe('Back');
    expect(displayKey('CAPTIONS')).toBe('Captions');
    expect(displayKey('DTS')).toBe('DTS');
    expect(displayKey('DPAD_UP')).toBe('D-Pad Up');
  });
});

describe('displayKey: Title Case', () => {
  it.each([
    ['PrevChannel', 'Previous Channel'], ['prev_channel', 'Previous Channel'], ['vol_up', 'Volume Up'], ['power_on', 'Power On'], ['PowerOff', 'Power Off'],
    ['power_toggle', 'Power Toggle'], ['channel_down', 'Channel Down'], ['skip_forward', 'Skip Forward'],
    ['home_control_1', 'Upper Bulb'], ['home_control_4', 'Lower Socket'],
    ['home_control_plus', 'Home Control +'], ['home_control_minus', 'Home Control −'],
    ['volume_down', 'Volume Down'], ['VolumeDown', 'Volume Down'], ['fast_forward', 'Fast Forward'], ['FastForward', 'Fast Forward'], ['red', 'Red'],
  ])('%s -> %s', (raw, want) => expect(displayKey(raw)).toBe(want));
  it('keeps small joining words lower-case unless first', () => {
    expect(displayKey('back_to_tv')).toBe('Back to TV');
    expect(displayKey('state_of_the_art')).toBe('State of the Art');
    expect(displayKey('of_note')).toBe('Of Note');
  });
  it('says plus and minus for screen readers', () => {
    expect(spokenKey('home_control_plus')).toBe('Home Control Plus');
    expect(spokenKey('home_control_minus')).toBe('Home Control Minus');
    expect(spokenKey('volume_up')).toBe('Volume Up');
  });
});

describe('displayKey', () => {
  it.each([
    ['select', 'OK'], ['Menu', 'Menu'], ['dvr', 'DVR'], ['DVR', 'DVR'], ['Num5', '5'], ['key_105', '5'], ['key_7', 'Key 7'],
    ['hdmi_input_2', 'HDMI Input 2'], ['direction_up', 'Up'], ['DirectionUp', 'Up'], ['direction_right', 'Right'], ['DirectionLeft', 'Left'],
  ])('%s -> %s', (raw, want) => expect(displayKey(raw)).toBe(want));
  it('never leaves underscores or a lower-case start', () => {
    for (const k of ['some_new_key', 'SomeNewKey', 'x_1', 'a']) {
      const t = displayKey(k);
      expect(t).not.toContain('_');
      expect(t[0]).toBe(t[0]?.toUpperCase());
    }
  });
  it('is what the remote and the Bluetooth views show', () => {
    expect(keyTitle('prev_channel')).toContain('Previous Channel');
    expect(keyLabel('volume_up')).toBe('Volume Up');
    expect(keyLabel('key_103')).toBe('Digit 3');
  });
});
