// Home Assistant entity -> a proposed device. Only what the domain's services can do is proposed;
// source lists and send_command presets are not readable from the entity list, so the user fills them in.
import type { Device, IrAction } from './model';
import { trimBytes } from './codes';
import { deviceIdFor } from './device-edit';
import type { Config } from './model';

export interface Proposal {
  device: Device;
  /** honest notes about what could not be known */
  notes: string[];
}
const act = (service: string, entity_id: string, data?: Record<string, unknown>): IrAction => ({
  ha: { service, target: { entity_id }, ...(data ? { data } : {}) },
});
export const PROPOSAL_DOMAINS = ['media_player', 'remote', 'switch', 'light', 'fan'] as const;

export function proposeFromEntity(cfg: Config, entity: { entity_id: string; name?: string }): Proposal | null {
  const id = entity.entity_id;
  const domain = id.split('.')[0] ?? '';
  const name = trimBytes(entity.name ?? id.split('.')[1] ?? id);
  const base: Device = { id: deviceIdFor(cfg, name), kind: 'ha', name, entity: id, power: { on: act(`${domain}.turn_on`, id), off: act(`${domain}.turn_off`, id) }, inputs: {}, commands: {} };
  const notes: string[] = [];
  if (domain === 'media_player') {
    const c = (service: string, data?: Record<string, unknown>) => act(`media_player.${service}`, id, data);
    base.commands = {
      vol_up: c('volume_up'),
      vol_down: c('volume_down'),
      mute: c('volume_mute', { is_volume_muted: true }),
      unmute: c('volume_mute', { is_volume_muted: false }),
      play_pause: c('media_play_pause'),
      play: c('media_play'),
      pause: c('media_pause'),
      stop: c('media_stop'),
      next: c('media_next_track'),
      prev: c('media_previous_track'),
    };
    notes.push('Home Assistant does not tell us this player\'s source list. Add each source as an input (it selects the source by name).');
    notes.push('Mute and unmute are separate: Home Assistant has no mute toggle.');
  } else if (domain === 'remote') {
    notes.push('A remote entity sends named commands (remote.send_command). Add the commands you use, for example "HOME" or "NETFLIX"; we cannot read the list from Home Assistant.');
  } else if (domain === 'switch' || domain === 'light' || domain === 'fan') {
    notes.push(`This device only has power. Add commands if the ${domain} accepts other services.`);
  } else return null;
  return { device: base, notes };
}

/** A command that sends a named `remote.send_command` to a remote entity. */
export const remoteCommand = (entity: string, command: string): IrAction => act('remote.send_command', entity, { command: [command] });
/** An input that selects a media_player source by name. */
export const sourceInput = (entity: string, source: string): IrAction => act('media_player.select_source', entity, { source });
